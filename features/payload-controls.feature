Feature: Payload controls survive streamed event replay
  The schemas in these scenarios are synthetic API exercises. They do not
  represent MIL-STD-6017 message definitions or fielded-system evidence.

  Scenario: A leading recurrence flag accompanies each body
    Given a synthetic leading recurrence body in a D1 envelope
    When I decode and replay its payload with single-octet buffers
    Then the recurrence body flags and values are preserved
    And the payload control replay matches every original octet

  Scenario: A known recurrence count supplies the continuation flags
    Given a synthetic leading recurrence body in a D1 envelope
    And the recurrence count is known before replay
    When I decode and replay its payload with single-octet buffers
    Then the recurrence body flags and values are preserved
    And the payload control replay matches every original octet

  Scenario: A previous scalar controls a choice and conditional presence
    Given a synthetic conditional choice body in a D1 envelope
    When I decode and replay its payload with single-octet buffers
    Then the choice bytes and conditional value are preserved
    And the payload control replay matches every original octet

  Scenario: Alternative choice arms cannot supply each other's values
    Given a payload schema with a dependency between alternative choice arms
    When I try to create codecs for that payload schema
    Then both codecs reject the schema before reading or writing

  Scenario: UTF-8 characters survive input fragmentation and chunk limits
    Given a synthetic UTF-8 text body in a D1 envelope
    When I decode and replay its payload with single-octet buffers
    Then the text chunks preserve complete characters and wire offsets
    And the payload control replay matches every original octet

  Scenario: Schema lookup supports caller-configured deep trees
    Given a valid payload schema with 4096 nested nodes
    Then schema lookup returns the deepest original node and rejects missing IDs
