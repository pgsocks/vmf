Feature: Bounded allocation and envelope-scoped measurement
  Scenario: Measure the fictitious payload before replaying its chunks
    Given the literal envelope for profile 4
    When I measure and encode the fictitious D1 payload in two chunks
    Then the status is DONE
    And the replay is byte identical

  Scenario: Allocation failure releases partial resources and permits recovery
    Given the literal envelope for profile 4
    When allocation fails once and then succeeds
