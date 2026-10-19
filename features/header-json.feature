@header_json
Feature: Public owned envelope header configuration
  Applications configure exact VMF identities without private CLI helpers.
  Optional header fields can be left out and errors identify their JSON path.

  Scenario: Minimal templates load for every explicit envelope profile
    Given an exact public header identity
    When I generate and load the public minimal header templates
    Then every loaded template preserves that identity

  Scenario: Profile-incompatible properties identify the configuration error
    Given a B header with a later-profile property
    When I load that header through the public JSON companion
    Then the public header error identifies the incompatible property
