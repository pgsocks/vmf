Feature: Applications supply values while VMF traverses schemas
  The native schemas and nested objects are synthetic API exercises.
  They do not represent assigned MIL-STD-6017 layouts.

  Scenario Outline: Measure, rewind and encode a nested caller object
    Given a caller-owned two-by-two object and its native VMF schema
    When I encode it with the public <interface> interface
    Then decoding returns all four nested values in schema order

    Examples:
      | interface      |
      | C11 primitive  |
      | C++17 callback |

  Scenario: Output exhaustion does not ask for the same reply twice
    Given a caller-owned two-by-two object and its native VMF schema
    When I stream its callback replies through one-octet output buffers
    Then the callback runs once for each of the four nested values
    And the streamed output matches the measured encoding

  Scenario: A provider pause waits for its exhausted reply to be consumed
    Given a caller-owned two-by-two object and its native VMF schema
    When its callback pauses while output is exhausted
    Then the pause is reported after consuming the cached reply
    And the streamed output matches the measured encoding

  Scenario: A provider abort requires reset before encoding resumes
    Given a caller-owned two-by-two object and its native VMF schema
    When its callback aborts and I reset the encoder
    Then the reset encoding asks once for each nested value
    And the streamed output matches the measured encoding
