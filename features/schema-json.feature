@schema_json
Feature: Native payload schemas loaded from JSON
  Applications can keep schemas in JSON and use the ordinary VMF codecs.

  Scenario Outline: A loaded schema matches its handwritten equivalent
    Given a JSON definition of a synthetic <payload> payload
    When I load the JSON payload schema and release its source bytes
    And I decode and replay the JSON payload using single-octet buffers
    Then the JSON schema produces the handwritten schema's events and bytes

    Examples:
      | payload                  |
      | scalar                   |
      | recurrence               |
      | presence                 |
      | absence                  |
      | field-count              |
      | counted optional group   |
      | group recurrence         |
      | group absence            |
      | nested recurrence        |
      | recurring optional field |
      | single-field group       |
      | named dependencies       |
      | UTF-8 text               |
      | literal text             |

  Scenario: Loading a schema set is atomic
    Given a JSON schema set with a valid entry followed by an invalid entry
    When I attempt to load the complete JSON schema set
    Then no schema owner is published and the second entry is diagnosed

  Scenario Outline: A nested schema failure identifies the authored property
    Given a nested JSON schema containing <problem>
    When I attempt to load the complete JSON schema set
    Then the failing authored property is diagnosed after source release

    Examples:
      | problem                     |
      | an unknown length reference |
      | a duplicate field key       |

  Scenario: One payload definition works with every envelope edition
    Given an envelope-independent JSON registry for MIL-STD-6017B
    When I encode and decode the same payload with B, C, D and D1 envelopes
    Then every envelope carries the same payload using the unchanged registry

  Scenario Outline: Envelope binding rejects unrepresentable identities
    Given an envelope-independent JSON registry for <definition>
    When I bind that registry to envelope <envelope>
    Then binding fails with <status> without changing the registry

    Examples:
      | definition                       | envelope | status            |
      | MIL-STD-6017E                    | C        | invalid_schema    |
      | two editions of the same message | B        | duplicate_message |
