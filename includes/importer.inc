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
  protected function getMungeKey() {
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
  protected function mungeKeyExists(&$errors = NULL) {
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

  /**
   * Convenience function to update and get the values for substitution.
   *
   * @param array $sandbox
   *   The $context['sandbox'] array, where we'll keep our stats.
   * @param string|bool $type_to_increment
   *   Either a boolean FALSE to just get the counts without incrementing, or
   *   a string indicating which value to update in the set:
   *   - "updated": An integer containing the running count of the number of
   *     records updated.
   *   - "created": An integer containing the running count of the number of
   *     records created.
   *
   * @return array
   *   An associative array containing placeholder substitutions for
   *   format_string() and associated functions:
   *   - "@updated": The number updated.
   *   - "@created": The number created.
   *   - "@total": The sum of the number updated and created.
   */
  protected function getCounts(&$sandbox, $type_to_increment = FALSE) {
    if (!isset($sandbox['counts'])) {
      $sandbox['counts'] = array(
        'updated' => 0,
        'created' => 0,
      );
    }

    if ($type) {
      $sandbox['counts'][$type_to_increment]++;
    }

    return array(
      '@updated' => $sandbox['counts']['updated'],
      '@created' => $sandbox['counts']['created'],
      '@total' => array_sum($sandbox['counts']),
    );
  }

  /**
   * {@inheritdoc}
   */
  public function writeToFedora($pid, $parent_pid, &$sandbox) {
    $report = array();

    // Check if the object exists, based on the munge key.
    $object = $this->mungeKeyExists($report);

    if ($this->getMungeKey() && $object) {
      // If it does exist, and we actually had a key to match, update the
      // label, PBCore and DC on the existing object.
      try {
        $object->label = $this->getTitle();
        $object['PBCORE']->content = $this->getPBCore();
        $object['DC']->content = $this->getDC();
        $report[] = array(
          t('(@created/@updated/@total) Successfully updated @pid for "@key".'),
          $this->incrementAndGetCounts($sandbox, 'updated') + array(
            '@pid' => $object->id,
            '@key' => $this->getMungeKey(),
          ),
          WATCHDOG_INFO,
          l($object->id, "islandora/object/{$object->id}"),
        );
      }
      catch (Exception $e) {
        $report[] = array(
          '%type: !message in %function (line %line of %file).',
          _drupal_decode_exception($e),
          WATCHDOG_WARNING,
        );
        $report[] = array(
          t('(@created/@updated/@total) Failed updating @pid from %file.'),
          $this->incrementAndGetCounts($sandbox, 'updated') + array(
            '@pid' => $object->id,
            '%file' => $this->source['object_info']['xml'],
          ),
          WATCHDOG_ERROR,
          l($object->id, "islandora/object/{$object->id}"),
        );
      }
    }
    else {
      // If it does not, create a new object (as per the parent
      // implementation?).
      // An array of Drupal file objects to clean up after ingest.
      $files = array();

      try {
        $label = $this->getTitle();
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

        $parent = islandora_object_load($collection_pid);
        if (isset($parent['POLICY']) && !isset($object['POLICY'])) {
          $object->ingestDatastream($parent['POLICY']);
        }

        if ($this->getMungeKey()) {
          $report[] = array(
            t('(@created/@updated/@total) Successfully created @pid for "@key" from %file.'),
            $this->incrementAndGetCounts($sandbox, 'created') + array(
              '@pid' => $object->id,
              '@key' => $this->getMungeKey(),
              '%file' => $this->source['object_info']['xml'],
            ),
            WATCHDOG_INFO,
            l($object->id, "islandora/object/{$object->id}"),
          );
        }
        else {
          $report[] = array(
            t('(@created/@updated/@total) Created @pid for %file, which is missing a munge key.'),
            $this->incrementAndGetCounts($sandbox, 'created') + array(
              '@pid' => $object->id,
              '%file' => $this->source['object_info']['xml'],
            ),
            WATCHDOG_WARNING,
            l($object->id, "islandora/object/{$object->id}"),
          );
        }
      }
      catch (Exception $e) {
        $report[] = array(
          '%type: !message in %function (line %line of %file).',
          _drupal_decode_exception($e),
          WATCHDOG_WARNING,
        );
        $report[] = array(
          t('(@created/@updated/@total) Failed creating new object from %file.'),
          $this->incrementAndGetCounts($sandbox, 'created') + array(
            '%file' => $this->source['object_info']['xml'],
          ),
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
