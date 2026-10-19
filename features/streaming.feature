Feature: Streaming input and event replay
  Scenario Outline: Fragmented input preserves events
    Given the literal envelope for profile <profile>
    And input arrives one octet at a time
    When I decode one complete PDU
    Then the status is DONE
    And exactly one PDU was consumed

    Examples:
      | profile |
      | 1       |
      | 2       |
      | 3       |
      | 4       |

  Scenario: Leave concatenated PDU bytes untouched
    Given the literal envelope for profile 4
    And one extra PDU follows
    When I decode one complete PDU
    Then the status is DONE
    And exactly one PDU was consumed

  Scenario: NEED_OUTPUT retries preserve the current event
    Given the literal envelope for profile 4
    And events are replayed with one octet of output capacity
    When I decode one complete PDU
    Then the status is DONE
    And the replay is byte identical

  Scenario: Pause consumes one event
    Given the literal envelope for profile 4
    When I dispatch with callback pause
    Then one callback was consumed and processing paused

  Scenario: Abort consumes one event
    Given the literal envelope for profile 4
    When I dispatch with callback abort
    Then one callback was consumed and processing aborted

  Scenario: Reset discards partial header state
    Given the literal envelope for profile 4
    When I reset after partial input and decode again
    Then the status is DONE
    And exactly one PDU was consumed

  Scenario: Header storage survives payload traversal and DONE
    Given the literal envelope for profile 4
    When I decode one complete PDU
    Then the status is DONE
    And the retained header still describes one VMF body

  Scenario: Resuming a paused callback does not replay the consumed envelope
    Given the literal envelope for profile 4
    When I dispatch with callback pause
    And I resume callback delivery
    Then the status is DONE
    And the envelope was delivered once among seven callbacks
