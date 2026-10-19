Feature: Author complete VMF PDUs from explicit raw values
  The schemas and values exercise the public API and are synthetic.
  Constants and scalar lengths or counts remain the caller's responsibility.

  Scenario: Publish one complete owned PDU with measured envelope sizes
    Given complete raw values with canonical nested VMF addresses
    When I author the complete PDU using one-octet output slices
    Then the owned PDU decodes to the complete nested values
    And the owned PDU size agrees with its measurement

  Scenario: A missing active mandatory value publishes no output
    Given complete raw values with canonical nested VMF addresses
    When I author the PDU with a missing mandatory nested value
    Then no PDU is published and the missing repeat occurrence is identified
