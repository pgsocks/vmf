Feature: Exact schema selection and balanced payload events
  Scenario Outline: Traverse a synthetic binary-file schema
    Given the literal envelope for profile <profile>
    When I decode one complete PDU
    Then the status is DONE
    And exactly one PDU was consumed

    Examples:
      | profile |
      | 1       |
      | 2       |
      | 3       |
      | 4       |

  Scenario: Missing version is not a wildcard
    Given the literal envelope for profile 4
    And an exact identity with another standard version
    When I decode one complete PDU
    Then the status is UNKNOWN_MESSAGE

  Scenario: Empty schema set still permits header inspection only
    Given the literal envelope for profile 4
    And no payload schemas
    When I decode one complete PDU
    Then the status is UNKNOWN_MESSAGE

  Scenario: Final exhaustion before the declared body is truncation
    Given the literal envelope for profile 2
    And the input ends before the body
    When I decode one complete PDU
    Then the status is TRUNCATED
