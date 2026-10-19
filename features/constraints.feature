Feature: Normative constraints are stronger than field widths
  Scenario: Reject a reserved precedence code
    Given the literal envelope for profile 4
    And the D1 precedence code is reserved
    When I inspect the envelope independently
    Then the status is MALFORMED

  Scenario: Retransmission requires the original DTG
    Given the literal envelope for profile 4
    And the D1 operation requests retransmission without an originator DTG
    When I inspect the envelope independently
    Then the status is MALFORMED
