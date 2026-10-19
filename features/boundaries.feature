Feature: Header metadata and body boundaries have separate contracts
  Scenario: Header inspection permits an omitted message length
    Given the literal envelope for profile 4
    And the literal missing variant
    When I inspect the envelope independently
    Then the status is DONE
    And only the header octets were consumed

  Scenario: Final input delimits a sole body without MessageSize
    Given the literal envelope for profile 4
    And the literal missing variant
    When I decode one complete PDU
    Then the status is DONE
    And exactly one PDU was consumed

  Scenario: Inspect compression metadata without a payload schema
    Given the literal envelope for profile 4
    And the literal compressed variant
    And no payload schemas
    When I inspect the envelope independently
    Then the status is DONE
    And only the header octets were consumed

  Scenario: Full traversal rejects a compressed body before consuming it
    Given the literal envelope for profile 4
    And the literal compressed variant
    When I decode one complete PDU
    Then the status is UNSUPPORTED_ENVELOPE_FEATURE
    And only the header octets were consumed

  Scenario: Acknowledgment descriptors need no payload schemas
    Given the literal envelope for profile 4
    And the literal ack variant
    When I decode one complete PDU
    Then the status is DONE
    And there were no payload events

  Scenario: Repeated descriptors delimit their respective bodies
    Given the literal envelope for profile 4
    And the literal multiple variant
    And input arrives one octet at a time
    When I decode one complete PDU
    Then the status is DONE
    And two body messages completed in descriptor order

  Scenario: Authentication metadata is inspectable without cryptographic work
    Given the literal envelope for profile 4
    And the literal security variant
    And no payload schemas
    When I inspect the envelope independently
    Then the status is DONE
    And only the header octets were consumed
    And the header exposes an unverified forty-octet signature
