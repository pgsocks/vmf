@schema_json
Feature: Native payload schemas loaded from JSON
  Applications can keep schemas in JSON and use the ordinary VMF codecs.

  Scenario Outline: A loaded schema matches its handwritten equivalent
    Given a JSON definition of a synthetic <payload> payload
    When I load the JSON payload schema and release its source bytes
    And I decode and replay the JSON payload using single-octet buffers
    Then the JSON schema produces the handwritten schema's events and bytes

    Examples:
      | payload    |
      | scalar     |
      | recurrence |
      | choice     |
      | UTF-8 text |

  Scenario: Loading a schema set is atomic
    Given a JSON schema set with a valid entry followed by an invalid entry
    When I attempt to load the complete JSON schema set
    Then no schema owner is published and the second entry is diagnosed
