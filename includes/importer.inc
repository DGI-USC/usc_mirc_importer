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
  protected function getIdentifier(IslandoraTuque $tuque) {
    $pid = NULL;

    if (isset($this->context['sandbox']['pid'])) {
      $object = islandora_object_load($this->context['sandbox']['pid']);
      if (!$object) {
        // The object failed to load, likely due to it not existing...
        // Requiring an ID to be provided to writeToFedora() seemed like a good
        // idea at the time. :P
        $pid = $this->context['sandbox']['pid'];
      }
    }

    if (!$pid) {
      $pid = $this->context['sandbox']['pid'] = $tuque->repository->api->m->getNextPid($this->pidNamespace);
    }

    return $pid;
  }
}

class USCMIRCBatchImportObject extends ZipBatchImportObject {
  protected $munge_key, $pbcore, $pbcore_simplexmlelement;

  /**
   * {@inheritdoc}
   */
  public function getTitle() {
    // Get the title for this object (From the PBCore?).
    if ($this->title === NULL) {
      $s_xml = $this->getPBCoreSimpleXMLElement();
      $titles = $s_xml->xpath('/pb:pbcoreDescriptionDocument/pb:pbcoreTitle[@titleType="Main"]');
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
    if ($this->pbcore_simplexmlelement === NULL) {
      $this->pbcore_simplexmlelement = new SimpleXMLElement($this->getPBCore());
      $this->pbcore_simplexmlelement->registerXPathNamespace('pb', 'http://www.pbcore.org/PBCore/PBCoreNamespace.html');
    }

    return $this->pbcore_simplexmlelement;
  }

  /**
   * Get the munge_fm_key identifier.
   */
  protected function getMungeKey() {
    if ($this->munge_key === NULL) {
      $s_xml = $this->getPBCoreSimpleXMLElement();
      $keys = $s_xml->xpath('/pb:pbcoreDescriptionDocument/pb:pbcoreIdentifier[@source="munge_fm_key"]');
      if ($keys) {
        $key = reset($keys);
        $this->munge_key = (string) $key;
      }
    }

    return $this->munge_key;
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
  protected function mungeKeyExists(&$errors = NULL) {
    $key = $this->getMungeKey();
    $model = reset($this->contentModel);

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
          t('Multiple object with key "@key". Only changing the first (by date; @pid).'),
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

    return FALSE;
  }

  /**
   * {@inheritdoc}
   */
  public function writeToFedora($pid, $parent_pid = NULL) {
    $report = array();

    // Check if the object exists, based on the munge key.
    $object = $this->mungeKeyExists($report);

    if ($object) {
      // If it does, update the PBCore and DC on the existing object.
      try {
        $this->getPBCoreSimpleXMLElement();
        $object['PBCORE']->content = $this->getPBCore();
        $object['DC']->content = $this->getDC();
      }
      catch (Exception $e) {
        $report[] = array(
          '%type: !message in %function (line %line of %file).',
          _drupal_decode_exception($e),
          WATCHDOG_ERROR,
        );
      }
    }
    else {
      // If it does not, create a new object (as per the parent
      // implementation?).
      // An array of Drupal file objects to clean up after ingest.
      $files = array();

      $label = $this->getTitle();
      try {
        $datastreams = $this->getDatastreams($report, $files);

        $relationships = array();

        $content_models = array_unique((array) $this->contentModel);

        $collection_pid = $parent_pid;
        $relationships[] = array(
          'relationship' => 'isMemberOfCollection',
          'pid' => $parent_pid,
        );
        module_load_include('inc', 'islandora', 'includes/utilities');
        $object = islandora_prepare_new_object($pid, $label,
                  $datastreams, $content_models, $relationships, $collection_pid);

        $object = islandora_add_object($object);
      }
      catch (Exception $e) {
        $report[] = array(
          '%type: !message in %function (line %line of %file).',
          _drupal_decode_exception($e),
          WATCHDOG_ERROR,
        );
      }

      foreach ($files as $file) {
        if ($file) {
          file_delete($file);
        }
      }
    }

    return $report;
  }
}
