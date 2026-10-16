@cli
Feature: Inspect, record, and replay VMF streams from pipes
  Binary stdout remains suitable for another process and diagnostics use stderr.

  Scenario: Discover command syntax
    When I exercise CLI help
    Then the CLI outcomes match the requested workflow

  Scenario: Inspect without writing binary stdout by default
    When I exercise CLI inspect
    Then the CLI outcomes match the requested workflow

  Scenario: Increase event detail through all verbosity levels
    When I exercise CLI verbosity
    Then the CLI outcomes match the requested workflow

  Scenario: Decode all supported envelope profiles
    When I exercise CLI profiles
    Then the CLI outcomes match the requested workflow

  Scenario: Observe fragmented input before the producer closes its pipe
    When I exercise CLI live-pipe
    Then the CLI outcomes match the requested workflow

  Scenario: Record editable events and replay identical bytes
    When I exercise CLI record-replay
    Then the CLI outcomes match the requested workflow

  Scenario: Edit a scalar without changing recorded message sizes
    When I exercise CLI edit-scalar
    Then the CLI outcomes match the requested workflow

  Scenario: Reject inconsistent recorded sizes before forwarding a PDU
    When I exercise CLI strict-sizes
    Then the CLI outcomes match the requested workflow

  Scenario: Validate event ordering and archive version
    When I exercise CLI invalid-archive
    Then the CLI outcomes match the requested workflow

  Scenario: Inspect unknown payloads only with the explicit header option
    When I exercise CLI headers-only
    Then the CLI outcomes match the requested workflow

  Scenario: Require exact and unambiguous payload schema selection
    When I exercise CLI schemas
    Then the CLI outcomes match the requested workflow

  Scenario: Decode an unsized PDU when the producer closes stdin
    When I exercise CLI unsized
    Then the CLI outcomes match the requested workflow

  Scenario: Diagnose malformed inputs and command arguments
    When I exercise CLI failures
    Then the CLI outcomes match the requested workflow

  Scenario: Respect recorded PDU timestamps when timing is requested
    When I exercise CLI timed
    Then the CLI outcomes match the requested workflow
