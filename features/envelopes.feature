Feature: Revision-specific independent envelope inspection
  Scenario Outline: Inspect a header without payload schemas
    Given the literal envelope for profile <profile>
    And no payload schemas
    When I inspect the envelope independently
    Then the status is DONE
    And only the header octets were consumed

    Examples:
      | profile |
      | 1       |
      | 2       |
      | 3       |
      | 4       |

  Scenario: Profile mismatch cannot silently select another revision
    Given the literal envelope for profile 4
    And the selected profile is 2
    When I inspect the envelope independently
    Then the status is UNSUPPORTED_ENVELOPE

  Scenario: Undefined versions do not opt into forward compatibility
    Given the literal envelope for profile 4
    And the wire version is 5
    When I inspect the envelope independently
    Then the status is UNSUPPORTED_ENVELOPE

  Scenario: Encoded header size must equal measured header size
    Given the literal envelope for profile 3
    And the header size is incorrect
    When I inspect the envelope independently
    Then the status is MALFORMED

  Scenario: Header padding must be zero
    Given the literal envelope for profile 4
    And the last header padding bit is one
    When I inspect the envelope independently
    Then the status is MALFORMED

  Scenario Outline: Shortened version-not-supported response
    Given the literal envelope for profile <profile>
    And a shortened response with originator 1 and recipient 2
    When I inspect the envelope independently
    Then the status is DONE
    And only the header octets were consumed

    Examples:
      | profile |
      | 2       |
      | 3       |
      | 4       |

  Scenario: B does not define version 15
    Given the literal envelope for profile 1
    And a shortened response with originator 1 and recipient 2
    When I inspect the envelope independently
    Then the status is UNSUPPORTED_ENVELOPE
