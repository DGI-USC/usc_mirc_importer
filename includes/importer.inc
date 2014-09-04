<?php

/**
 * @file
 * Importer class implementations.
 */

class USCMIRCBatchImporter extends ZipBatchImporter {
  protected $itemClass = 'USCMIRCBatchImportObject';

  /**
   * {@inheritdoc}
   */
  public static function getForm(array &$form_state) {
    // Make a single file upload.
    return array(
      'fs' => array(
        '#type' => 'fieldset',
        '#title' => t('PBCore Batch Importer'),
        '#description' => t('Select the file containing the metadata to import.'),
        'file' => array(
          '#type' => 'managed_file',
          '#title' => t('Zip file containing files to import'),
          '#upload_validators' => array(
            'file_validate_extensions' => array('zip'),
          ),
        ),
        'submit' => array(
          '#type' => 'submit',
          '#value' => t('Import'),
        ),
      ),
    );
  }

  /**
   * {@inheritdoc}
   */
  public static function readyForBatch(array &$form_state) {
    // Got a file?
    if (empty($form_state['values']['file'])) {
      form_set_error('file', t('Need a Zip file!'));
      return FALSE;
    }
    return TRUE;
  }

  /**
   * {@inheritdoc}
   */
  public static function getBatchInfo(array &$form_state) {
    $file = file_load($form_state['values']['file']);
    return array(
      'file' => $file,
      'pid_namespace' => 'usc',
      'content_model' => array(variable_get('usc_mirc_cmodel_vro', 'usc:vroCModel')),
      'object_info' => static::getIngestInfo($file),
      'processed_object' => array(),
    );
  }

  /**
   * Get an ID, with some shenanigans to avoid wasting identifiers.
   *
   * @see IslandoraBatchImporter::getIdentifier()
   */
  protected function getIdentifier(IslandoraTuque $tuque, $namespace) {
    $pid = NULL;

    if (isset($this->context['sandbox']['pid'][$namespace])) {
      $object = islandora_object_load($this->context['sandbox']['pid'][$namespace]);
      if (!$object) {
        // The object failed to load, likely due to it not existing...
        // Requiring an ID to be provided to writeToFedora() seemed like a good
        // idea at the time. :P
        $pid = $this->context['sandbox']['pid'][$namespace];
      }
    }

    if (!$pid) {
      $pid = $this->context['sandbox']['pid'][$namespace] = $tuque->repository->api->m->getNextPid($namespace);
    }

    return $pid;
  }
}

class USCMIRCBatchImportObject extends ZipBatchImportObject {
  protected $mungeKey;
  protected $pbcore;
  protected $pbcoreSimpleXMLElement;

  /**
   * {@inheritdoc}
   */
  public function getWrapperClass() {
    return 'USCMIRCBatchObject';
  }

  /**
   * {@inheritdoc}
   */
  public function getTitle() {
    // Get the title for this object (From the PBCore?).
    if ($this->title === NULL) {
      $s_xml = $this->getPBCoreSimpleXMLElement();
      $titles = $s_xml->xpath('/pb:pbcoreDescriptionDocument/pb:pbcoreTitle[@titleType="Main"]');
      if ($titles) {
        $title = reset($titles);
        $this->title = (string) $title;
      }
    }

    return $this->title;
  }

  /**
   * {@inheritdoc}
   */
  protected function getDatastreams(&$errors = NULL, &$files = NULL) {
    // Get PBCore and DC datastreams.
    $datastreams = array();

    // Ingest the PBCore.
    $obtained_pbcore = $this->getPBCore();
    if ($obtained_pbcore) {
      $name = file_create_filename('PBCore record.xml', 'public://');
      $files[] = $file = file_save_data($obtained_pbcore, $name);
      $datastreams[] = array(
        'dsid' => 'PBCORE',
        'label' => 'PBCore Record',
        'mimetype' => 'text/xml',
        'control_group' => 'M',
        'datastream_file' => file_create_url($file->uri),
      );
    }
    else {
      $errors[] = array(
        t('Failed to produce PBCore record for @pid.'),
      );
    }

    // Ingest the DC second.
    $obtained_dc = $this->getDC();
    if ($obtained_dc) {
      $name = file_create_filename('DC record.xml', 'public://');
      $files[] = $file = file_save_data($obtained_dc, $name);
      $datastreams[] = array(
        'dsid' => 'DC',
        'label' => 'DC Record',
        'mimetype' => 'text/xml',
        'control_group' => 'X',
        'datastream_file' => file_create_url($file->uri),
      );
    }
    else {
      $errors[] = array(
        t('Failed to produce DC record for @pid.'),
      );
    }

    return $datastreams;
  }

  /**
   * Grab the XML, assuming that it's PBCore.
   */
  public function getPBCore() {
    // Get/generate the PBCore text.
    if ($this->pbcore === NULL) {
      if (isset($this->source['object_info']['xml'])) {
        $zip = new ZipArchive();
        $zip->open(drupal_realpath($this->source['file']->uri));
        $this->pbcore = $zip->getFromName($this->source['object_info']['xml']);
        $zip->close();
      }
    }

    return $this->pbcore;
  }

  /**
   * Get the PBCore as a SimpleXMLElement.
   *
   * @return SimpleXMLElement
   *   A SimpleXMLElement representing the given PBCore, with the XPath prefix
   *   already registered for convenience.
   *
   * @throws Exception
   *   Throws an Exception if the XML fails to parse.
   */
  protected function getPBCoreSimpleXMLElement() {
    if ($this->pbcoreSimpleXMLElement === NULL) {
      @$this->pbcoreSimpleXMLElement = new SimpleXMLElement($this->getPBCore());
      $this->pbcoreSimpleXMLElement->registerXPathNamespace('pb', 'http://www.pbcore.org/PBCore/PBCoreNamespace.html');
    }

    return $this->pbcoreSimpleXMLElement;
  }

  /**
   * Get the munge_fm_key identifier.
   */
  public function getMungeKey() {
    if ($this->mungeKey === NULL) {
      try {
        $s_xml = $this->getPBCoreSimpleXMLElement();
        $keys = $s_xml->xpath('/pb:pbcoreDescriptionDocument/pb:pbcoreIdentifier[@source="munge_fm_key"]');
        if ($keys) {
          $key = reset($keys);
          $this->mungeKey = (string) $key;
        }
      }
      catch (Exception $e) {
        // Squash...
      }
    }

    return $this->mungeKey;
  }

  /**
   * {@inheritdoc}
   */
  public function getDC() {
    if ($this->dc === NULL) {
      $new_dc = static::runXSLTransform(array(
                  'xsl' => drupal_get_path('module', 'usc_mirc') . '/transforms/pbcore_to_dc.xsl',
                  'input' => $this->getPBCore(),
                ));
      if ($new_dc) {
        $this->dc = $new_dc;
      }
    }

    return $this->dc;
  }

  /**
   * Determine if an object with the given key exists.
   *
   * @return AbstractObject|bool
   *   The loaded object, or boolean FALSE if we could not find a match.
   */
  public function mungeKeyExists(&$errors = NULL) {
    $content_model = reset($this->contentModel);

    $key = $this->getMungeKey();
    if ($key) {
      $query = <<<EOQ
SELECT ?object ?date
WHERE {
  ?object <dc:identifier> "$key" ;
          <fedora-model:hasModel> <info:fedora/$content_model> ;
          <fedora-model:state> <fedora-model:Active> ;
          <fedora-model:createdDate> ?date .
}
ORDER BY ?date
EOQ;

      $tuque = islandora_get_tuque_connection();

      $results = $tuque->repository->ri->sparqlQuery($query);
      $num_results = count($results);

      if ($num_results > 0) {
        $result = reset($results);
        $pid = $result['object']['value'];

        if ($num_results > 1) {
          $errors[] = array(
            t('Multiple objects with key "@key". Only changing the first (by date; @pid).'),
            array(
              '@key' => $key,
              '@pid' => $pid,
            ),
            WATCHDOG_WARNING,
            l($pid, "islandora/object/$pid"),
          );
        }

        $object = islandora_object_load($pid);
        if ($object) {
          return $object;
        }
      }
    }

    return FALSE;
  }
}

class USCMIRCBatchObject extends IslandoraImporterBatchObject {
  const FAKE_PID_PREFIX = 'usc-mirc-fake-pid';

  /**
   * Constructor.
   */
  public function __construct(IslandoraTuque $connection, IslandoraImportObject $object, &$preprocessor_parameters = array()) {
    parent::__construct($connection, $object, $preprocessor_parameters);
    if (!isset($preprocessor_parameters['usc_mirc_info'])) {
      $preprocessor_parameters['usc_mirc_info'] = array(
        'time' => time(),
        'count' => 0,
      );
    }
    global $user;
    $this->id = t('!prefix-!time-!uid:!count', array(
      '!prefix' => static::FAKE_PID_PREFIX,
      '!time' => $preprocessor_parameters['usc_mirc_info']['time'],
      '!uid' => $user->uid,
      '!count' => $preprocessor_parameters['usc_mirc_info']['count']++,
    ));
  }

  /**
   * Override batch processing function.
   *
   * We need to handle the case of updating existing objects, as identified by
   * the "munge" key.
   *
   * @see IslandoraImporterBatchObject::batchProcess()
   */
  public function batchProcess() {
    $report = array();
    $to_return = ISLANDORA_BATCH_STATE__READY;

    // Check if the object exists, based on the munge key.
    $object = $this->importerObjectInstance->mungeKeyExists($report);
    $source = $this->importerObjectInstance->getSource();

    if ($this->importerObjectInstance->getMungeKey() && $object) {
      // If it does exist, and we actually had a key to match, update the
      // label, PBCore and DC on the existing object.
      try {
        $object->label = $this->importerObjectInstance->getTitle();
        $object['PBCORE']->content = $this->importerObjectInstance->getPBCore();
        $object['DC']->content = $this->importerObjectInstance->getDC();

        // Need to avoid ingesting this object...
        $to_return = ISLANDORA_BATCH_STATE__NOT_READY;
        $report[] = array(
          t('Successfully updated @pid for "@key".'),
          array(
            '@pid' => $object->id,
            '@key' => $this->importerObjectInstance->getMungeKey(),
          ),
          WATCHDOG_INFO,
          l($object->id, "islandora/object/{$object->id}"),
        );
      }
      catch (Exception $e) {
        // Need to avoid ingesting this object...
        $to_return = ISLANDORA_BATCH_STATE__ERROR;
        $report[] = array(
          '%type: !message in %function (line %line of %file).',
          _drupal_decode_exception($e),
          WATCHDOG_WARNING,
        );
        $report[] = array(
          t('Failed updating @pid from %file.'),
          array(
            '@pid' => $object->id,
            '%file' => $source['object_info']['xml'],
          ),
          WATCHDOG_ERROR,
          l($object->id, "islandora/object/{$object->id}"),
        );
      }
    }
    else {
      try {
        $this->label = $this->importerObjectInstance->getTitle();
        list($datastreams, $errors, $files) = $this->importerObjectInstance->getDatastreamInfo();
        foreach ($datastreams as $ds) {
          $ds_uri = isset($ds['file']) ?
            $ds['file']->uri :
            $ds['datastream_file'];

          $datastream = $this->constructDatastream($ds['dsid'], $ds['control_group']);
          $datastream->label = $ds['label'];
          $datastream->mimetype = $ds['mimetype'];
          $datastream->setContentFromFile($ds_uri);
          $this->ingestDatastream($datastream);
        }

        $parent = islandora_object_load($this->preprocessorParameters['parent_pid']);
        if (isset($parent['POLICY']) && !isset($this['POLICY'])) {
          $this->ingestDatastream($parent['POLICY']);
        }

        if (!isset($this->id) || strpos($this->id, static::FAKE_PID_PREFIX) === 0) {
          // No PID, or a fake one allocated... Get a real one!
          $this->id = $this->repository->api->m->getNextPid('usc');
        }

        if ($this->importerObjectInstance->getMungeKey()) {
          $report[] = array(
            t('Creating @pid for "@key" from %file.'),
            array(
              '@pid' => $this->id,
              '@key' => $this->importerObjectInstance->getMungeKey(),
              '%file' => $source['object_info']['xml'],
            ),
            WATCHDOG_INFO,
            l($this->id, "islandora/object/{$this->id}"),
          );
        }
        else {
          $report[] = array(
            t('Creating @pid for %file, which is missing a munge key.'),
            array(
              '@pid' => $this->id,
              '%file' => $source['object_info']['xml'],
            ),
            WATCHDOG_WARNING,
            l($this->id, "islandora/object/{$this->id}"),
          );
        }
        $to_return = ISLANDORA_BATCH_STATE__DONE;
      }
      catch (Exception $e) {
        $to_return = ISLANDORA_BATCH_STATE__ERROR;
        $report[] = array(
          '%type: !message in %function (line %line of %file).',
          _drupal_decode_exception($e),
          WATCHDOG_WARNING,
        );
        $report[] = array(
          t('Failed creating new object from %file.'),
          array(
            '%file' => $source['object_info']['xml'],
          ),
          WATCHDOG_ERROR,
        );
      }
    }

    foreach ($report as $message) {
      array_unshift($message, 'usc_mirc_importer');
      call_user_func_array('watchdog', $message);
    }

    return $to_return;
  }
}
