# MIL-STD-2045-47001 parser reference

This reference defines the envelope contract for profiles B, C, D and D with
Change 1 (D1). The [local requirement matrix](conformance.md) separates wire
validation from caller responsibilities. Public independent fixtures establish
limited C/D1 interoperability evidence; they do not certify a fielded message set.

## Sources and scope

The primary source is [ASSIST's revision archive][assist]: B, 20 January 1998;
C, 22 March 2002; D, 29 September 2005; D with Change 1, 23 June 2008.
Table I (B pp. 7–10, C pp. 7–12, D pp. 10–16, D1 pp. 15–22) and sections
5.3–5.6 define layout and field values; section 5.7.2 defines conditionality
in all four editions.

The [one-page A placeholder][a], dated 31 December 1997, explicitly says A
was never issued. It records the intent to give MIL-STD-2045-47001B the same
revision letter as MIL-STD-188-220B. It defines no technical format, and there
is no profile A. The separate [27 July 1995 edition][base] is unsupported;
its packing rules, unresolved header/body boundary and historical evidence
are preserved in the [historical note](#original-1995-edition) below.

The [DFDL comparison project][dfdl] supplies convenient public copies of
[B][b], [C][c] and [D1][d1]. It is secondary evidence, not the specification.
The current ASSIST entry also lists later editions E and F; neither belongs to
this delivery. MIL-STD-6017 message definitions, transport framing, compression
algorithms and cryptographic operations are outside scope. The envelope can
identify these formats without knowing how to traverse their bodies.

## Notation, packing and controls

`uN` is an unsigned N-bit integer. Fields concatenate without alignment unless
an explicit pad appears below. Transmit each integer's least significant bit
first; successive stream bits occupy octet bits 0 through 7. ASCII characters
are seven-bit values with this same significance order. The last header octet
is zero-filled to eight bits; each body separately ends on an octet boundary
with zero fill (B/C/D/D1 sections 5.3, 5.5.6 and 5.6.9). Do not insert a gap
before a 24-bit URN, length, security span or literal.

`F(x)` = `presence:u1` followed by x only when presence=1.
`G(x)` has identical packing but controls a group. Absent groups have no child
indicators. `R(x,max)` = one or more `[more:u1, x]`, where more=1 promises
another occurrence and more=0 marks this occurrence as last. This is a leading
recurrence indicator describing what follows the current occurrence, not a
terminator after the item. An optional repeat is `G(R(...))`; its GPI appears
only once. FRI uses the same rule for repeated fields. No extra terminating
zero follows a last occurrence. Counts must stay within the stated maxima.
See all editions, sections 5.5.1–5.5.4.

`text(n)` reads at most n seven-bit ASCII characters. DELETE (127) terminates
before n and is not returned as data. At exactly n characters there is no
terminator. Spaces, punctuation and ASCII controls other than DELETE are not
implicit terminators. Empty text is DELETE immediately. All readers must
support both maximum-length and terminated forms (5.5.5, 5.6.3.2, 5.6.8,
5.6.25). Header text is distinct from UTF-8 payload event chunks.

The native payload schema API supports VMF (UMF 2) message/index definitions.
The envelope grammar and inspection interface continue to expose the other
formats listed below. Historical synthetic binary-file vectors remain
envelope evidence; they do not imply a public non-VMF payload schema API.
Envelope selection and message-standard edition selection remain distinct.

## Ordered grammar

These productions spell every ordinary header field in wire order. Names in
parentheses identify logical groups, not additional bits. `pad` means 0–7 zero
bits required to reach the next octet. A full PDU is Header followed by each
body-bearing descriptor's body, in descriptor order. There is no body count
or total-PDU-length field independent of the header's descriptors.

```text
Address       = F(urn:u24), F(name:text(64))
Originator    = G(Address)
Recipients    = G(R(Address,16))
Information   = G(R(Address,16 - recipient_count))
DTG           = year:u7, month:u4, day:u5, hour:u5, minute:u6, second:u6
ExtendedDTG   = DTG, F(extension:u12)
Identification= G(fad:u4, message_number:u7, F(subtype:u7))
Requests      = G(machine:u1, operator_ack:u1, operator_reply:u1)
Response      = G(ExtendedDTG, receipt_compliance:u3,
                  F(cantco:u3), F(cantpro:u6), F(amplification:text(50)))
ReferencesB   = G(R(Address, ExtendedDTG, fad:u4, message_number:u7; max=4))
ReferencesCD  = G(R(Address, ExtendedDTG; max=4))
Future        = G(size:u12, opaque_bits[size])

HeaderB = version:u4=1, F(compression:u2), Originator,
          Recipients, Information, R(HandlingB,16), pad
HandlingB = umf:u4, Identification, F(file_name:text(64)), F(size:u20),
            operation:u2, retransmit:u1, precedence:u3, classification:u2,
            F(marking:u14), G(ExtendedDTG), G(DTG), Requests, Response,
            ReferencesB

HeaderC = version:u4=2, F(compression:u2), Originator,
          Recipients, Information, F(header_size:u16), R(HandlingC,16), pad
HandlingC = umf:u4, F(message_standard_version:u4), Identification,
            F(file_name:text(64)), F(size:u20), operation:u2, retransmit:u1,
            precedence:u3, classification:u2, F(marking:text(32)),
            G(ExtendedDTG), G(DTG), Requests, Response, ReferencesCD, Security

HeaderD = version:u4=3, F(compression:u2), Originator,
          Recipients, Information, F(header_size:u16),
          Future1, Future2, Future3, Future4, Future5,
          R(HandlingD,16),
          Future11, Future12, Future13, Future14, Future15, pad
HeaderD1 = HeaderD with version:u4=4
HandlingD = umf:u4, F(message_standard_version:u4), Identification,
            F(file_name:text(64)), F(size:u20), operation:u2, retransmit:u1,
            precedence:u3, classification:u2, F(R(marking:u9,16)),
            G(ExtendedDTG), G(DTG), Requests, Response, ReferencesCD,
            Future6, Future7, Future8, Future9, Future10, Security

ShortResponse = version:u4=15, compression_presence:u1=0,
                Originator, Recipients, pad
```

The two `G(DTG)` occurrences in handling productions mean originator and
perishability respectively; only originator uses ExtendedDTG. Recipient and
information addresses have a combined maximum of 16. Address URN and name are
mutually exclusive; both absent is structurally representable but violates the address
conditionality (5.7.2.2.3/.5). Version 15 requires originator and recipient
groups present, identifies the original destination as the responder, and has
no Information GPI, header size, descriptors or body. C/D/D1 permit it;
B's version table does not define it, so the B profile rejects it. See C/D/D1
5.6.1; C 5.7.4.2; D/D1 5.7.2.5.2. The specification says which fields are required rather
than presenting a separate complete bit diagram for this shortened form;
the production above records this library's interpretation of that rule.

D and D1 retain all fifteen Future GPIs, but require them to be zero for
versions 3 and 4. Their size-bearing grammar is shown to explain placement,
not to authorize future editions under an explicitly selected profile.
A future group's 12-bit size excludes its own size field and counts bits,
not octets. A zero size is discouraged by 5.6.42, rather than categorically
forbidden there. See D/D1 5.5.6.4 and 5.7.2.1.9.

## Security metadata

C/D/D1 use this ordered grammar (C groups G10–G16; D/D1 G20–G26).
B has no security group. Binary spans below are consecutive bits at the current
bit position, even though their logical lengths are multiples of octets.

```text
Security = G(spi:u4,
             G(key_id_length:u3, key_id:octets[key_id_length+1]),
             G(iv_length:u4, iv:octets[8*(iv_length+1)]),
             G(token_length:u8,
               R(token:octets[8*(token_length+1)],17)),
             G(auth_a_length:u7, auth_a:octets[8*(auth_a_length+1)]),
             G(auth_b_length:u7, auth_b:octets[8*(auth_b_length+1)]),
             signed_ack_request:u1,
             G(padding_length:u8, F(padding:octets[padding_length])))
```

Key ID: 1–8 octets. IV: 8–128 octets. Each token: 8–2048 octets, all sharing
the one token length. Auth A and B: 8–1024 octets. Security padding: 0–255
octets, with independent data FPI preserved. Length codes for key ID, IV,
tokens and authentication are zero-based. Security padding length is direct,
not zero-based. These are normative capacities (5.6.29–5.6.41), not Table XII's
smaller *typical* sizes.

SPI=0 defines authentication with SHA-1/DSA and no encryption; 1–15 are
undefined in these editions (Table XI). SPI=0 requires Auth A and excludes key
ID, IV, tokens and security padding (5.7.2.2.13). Appendix D specifies the
320-bit signature used by SPI=0; this is 40 octets and encodes length code 4.
Auth B requires Auth A and a signed acknowledgment; a signed acknowledgment
has Response present, no perishability, no requests, no body, and no signed
request bit (5.7.2.1.7). Parsing metadata does not authenticate it. Undefined
SPI values remain inspectable as metadata but do not authorize plaintext
traversal. Full processing rejects them with UNSUPPORTED_ENVELOPE_FEATURE.
SPI 0 allows plaintext traversal; its signatures remain unverified.

## Codes and scalar constraints

Unless stated otherwise these are common to the supported editions; revision
changes below are significant. Code tables name wire values, not enums whose
ordinal can be reordered. Primary references are 5.6 and its named tables.

| Field | Defined values / restrictions |
| --- | --- |
| Version | B=1, C=2, D=3, D1=4; 0 designates the unsupported 1995 edition in later tables (see historical note below); 5–14 undefined; 15 shortened response in C/D/D1 |
| Compression | absent=none, 0=LZW, 1=LZ-77 (D/D1 call this GZIP; C cites RFC 1951/1952), 2–3 undefined (Table III) |
| UMF | 0 Link16, 1 binary file, 2 VMF, 3 NITFS, 4 redistributed PDU, 5 USMTF, 6 DOI-103; C/D/D1 add 7 XML-MTF and 8 XML-VMF; remaining values undefined |
| Message standard version | absent in B; 4-bit code interpreted jointly with UMF, never inferred from header version; C/D/D1 Table V |
| FAD / number / subtype | 4/7/7 bits; number 1–127, zero illegal; meaning selected by UMF; assigned VMF definitions belong to MIL-STD-6017, not this envelope codec |
| Operation | 0 operation, 1 exercise, 2 simulation; B code 3 undefined, C/D/D1 code 3 test |
| Retransmit | 0 original, 1 retransmission |
| B/C precedence | 2 emergency command, 4 flash, 5 immediate, 6 priority, 7 routine; 0, 1, 3 undefined |
| D/D1 precedence | 0 routine, 1 priority, 2 immediate, 3 flash, 4 flash override, 5 CRITIC/ECP; 6–7 reserved |
| Classification | 0 unclassified, 1 confidential, 2 secret, 3 top secret |
| B control/release | Two seven-bit ASCII characters (14 bits), no terminator (B 5.6.14) |
| C control/release | up to 32 ASCII characters, DELETE termination (C 5.6.14) |
| D/D1 control/release | up to 16 recurring 9-bit codes (D/D1 5.6.14), assignments from MIL-STD-6017 |
| Year | 0–94=2000–2094; 95–99=1995–1999; 100–127 undefined |
| Month / day | 1–12 / 1–31; zero and unused codes illegal |
| Hour / minute | 0–23 / 0–59; unused codes illegal |
| Second | B/C: 0–59 only; D/D1: 0–59 or 63=no statement; unused codes illegal |
| DTG extension | 0–4095; needed to distinguish equal originator DTGs |
| R/C | 1 machine receipt, 2 cannot process, 3 operator acknowledgment, 4 will comply, 5 have complied, 6 cannot comply; 0/7 undefined |
| CANTCO / CANTPRO | 3/6-bit codes dependent on UMF; VMF assignments in Appendix A; not an unrestricted reason string |
| URN | 24 bits; C/D/D1 assign 16777215 to broadcast (not acknowledged); other assignments external |

Message-standard codes are revision-specific mappings, not monotonically
ordered library versions. The complete Table V interpretation is:

| UMF | C | D/D1 |
| --- | --- | --- |
| 0 Link16 | code 0=6016, 1–9=6016A–I; 10–15 reserved | code 0=6016, 1–15=6016A–O |
| 1 binary / 4 redistributed | every present version code illegal | every present version code illegal |
| 2 VMF | 0–5=TIDP-TE R2–R7; 6=6017, 7=6017A, 8=6017B, 9=6017C; 10–15 reserved | 0–3=TIDP-TE R2–R5, 4=TIDP-FTE R6, 5=6017, 6–10=6017A–E; 11–15 reserved |
| 3 NITFS | all undefined | 0=2500B Notice 2; 1–15 undefined |
| 5 USMTF | 0=1993, 1=1995, 2=1997, 3–9=1998–2004; 10–15 reserved | 0=1993, 1=1995, 2=1997, 3–15=1998–2010 |
| 6 DOI-103 | all undefined | all undefined |
| 7 XML-MTF | 0–5 undefined, 6–9=2001–2004; 10–15 reserved | 0–5 undefined, 6–15=2001–2010 |
| 8 XML-VMF | 0–2 undefined, 3–9 same as VMF; 10–15 reserved | 0–2 undefined, 3–10 same as VMF; 11–15 reserved |

Preserve the raw code and validate using the selected edition and UMF;
never upgrade a schema by treating a missing version as a wildcard. C's
construction example still labels Version=1 (B); its Table II defines C=2.
Use Table II for profile validation, not that inherited example.

The envelope's own VMF code assignments are also needed without importing
message body definitions. B Table A-1, C Table XIV and D/D1 Table A-I assign
FAD 0 network control, 1 general information exchange, 2 fire support,
3 air operations, 4 intelligence, 5 land combat, 6 maritime, 7 combat service
support, 8 special operations, 9 JTF operations control and 10 air defense/air
space control; 11–15 are undefined.

CANTCO reasons (B Table A-2, C Table XVI, D/D1 Table A-III) are 0 communications,
1 ammunition, 2 personnel, 3 fuel, 4 terrain/environment, 5 equipment,
6 tactical situation, 7 other. CANTPRO reasons have these assignments:

| Code | Meaning |
| --- | --- |
| 0 | Undefined |
| 1 | Field content invalid |
| 2 | Message incorrectly routed |
| 3 | Address inactive |
| 4 | Reference point unknown |
| 5 | Fire units must be controlled by receiving agency |
| 6 | Mission must be controlled by receiving agency |
| 7 / 8 / 9 | Mission / target / schedule number unknown |
| 10 | Incorrect controlling address for track number |
| 11 | Track not in own file |
| 12 | Invalid according to given field |
| 13 | Message cannot be converted |
| 14 | Agency file full |
| 15 | Message number not recognized |
| 16 | Cannot correlate to current file |
| 17 | Repeated field/group limit exceeded |
| 18 | Computer inactive |
| 19 | Unknown addressee |
| 20 / 21 | Cannot forward: agency / link failure |
| 22 | Illogical header fields |
| 23 / 24 | Cannot decompress LZW / LZ-77 |
| 25 | Too old by perishability |
| 26 | Security level restriction (C/D/D1) |
| 27 | Authentication failure (C/D/D1) |
| 28 / 29 | Certificate not found / invalid (C/D/D1) |
| 30 | Unsupported SPI (C/D/D1) |
| 31 | Cannot generate signed acknowledgment (C/D/D1) |
| 32 | Response unavailable for retransmission (D/D1) |

B Table A-3 leaves 26–63 undefined; C Table XVII leaves 32–63 undefined;
D/D1 Table A-IV leaves 33–63 undefined. These assignments apply to VMF;
other UMFs need their own associated standards. Subtype assignments and release
country codes remain external to this parser reference because their defining
body standard is explicitly outside this delivery.

## Conditionality, lengths and body association

The wire grammar alone is insufficient validation. Apply each edition's
5.7.2 cases and conditions after parsing the relevant dependencies:

- Ordinary messages have Response absent and a body. Acknowledgments have
  Response present, no body, no perishability and no acknowledgment requests.
  A handling descriptor therefore does not imply a body or a schema lookup.
- VMF requires identification; XML-VMF also requires it in C/D/D1. Binary,
  redistributed, USMTF, DOI-103, XML-MTF and other non-VMF formats exclude it
  as specified by their edition's cases. Subtype cannot exist without its
  identification group. D/D1 VMF requires a message-standard version.
- Binary file transfer requires file name and message size. D/D1 DOI-103 uses
  precedence 5. Compression applies independently to every body and sizes
  count compressed octets, not uncompressed lengths.
- B additionally excludes Information when Recipients is absent
  (5.7.2.2.6). Later editions' broadcast rule depends on both groups absent.
- R/C other than 6 excludes CANTCO reason; R/C other than 2 excludes CANTPRO
  reason. B additionally excludes amplification unless R/C is 4, 5 or 6;
  C/D/D1 leave that former condition blank. Any requested acknowledgment
  requires an originator DTG. D/D1 explicitly require it for retransmission.
  An absent originator address excludes the entire request group, even if all
  request bits are zero. A signed request requires the request group.
  Acknowledgment headers require originator and recipient address groups.
  Sender/recipient history constraints, response routing, clock expiry and
  automatic replies belong to the caller.
- Repeated handling groups require MessageSize (and C/D/D1 HeaderSize).
  HeaderSize includes every header field and padding, including itself.
  MessageSize is a 20-bit octet count, including the body's last padded octet.
  It is not an overall PDU length or a count of header bits.
- A single delimited message can legally omit sizes. Independent envelope
  parsing accepts those omissions. For a sole size-omitting descriptor, the API
  accepts input.final as the enclosing-input boundary. MESSAGE_BEGIN reports
  unknown length and MESSAGE_END reports actual padded length. Until final,
  incomplete input returns NEED_INPUT. Do not concatenate another PDU after
  that unsized body in the same input. Explicit sizes delimit concatenated PDUs
  without consuming the next header. Acknowledgment-only and shortened PDUs
  have no body to bound.

A present encoded HeaderSize must equal measured padded header octets. Compute
PDU size only when all body-bearing descriptors have sizes: header octets plus
their sizes. Descriptor indexes include acknowledgment-only descriptors; do
not compact them when associating measurements, events or errors.

B 5.7.5.5 caps the entire concatenated block, including the header, at
1,048,575 octets. C has conflicting size statements: 5.7.4.6 speaks of an
individual user-data portion and also says "including application header";
5.8.1 caps the total block with a lower-layer pass-through exception. This API
has no transport/pass-through configuration. Its conservative C policy is to
cap the whole PDU at 1,048,575 octets as well as enforcing 20-bit body sizes.
D/D1 state the individual-body bound explicitly. Configured resource limits can
be stricter than these wire/profile limits, never loosen them.

## Source conflicts and chosen policies

Normative field definitions and mandatory conditions take precedence over
nonmandatory Appendix B examples and initial-setting recommendations. In
particular D1 Figure 2 omits the message-standard version for VMF whereas
5.7.2.2.18 requires it. A literal copied from an example is not automatically
valid under strict validation. The case text around XML/non-XML categories is
also awkward: use the explicitly enumerated UMF values and the separate
XML-VMF case, not the case heading as a new rule.

The standard defines circumstances where a sender may omit lengths because
a lower layer delimits a PDU; this library represents that boundary with
input.final for a sole unsized descriptor. That flag must describe the end of
the enclosing input, not an arbitrary currently empty buffer. Explicit-profile
rejection of future versions is a library scope decision; D's forward-reading
procedure is not implemented as automatic profile switching.

B calls compression code 1 LZ-77 without specifying GZIP framing as D/D1 do;
the reference does not retroactively impose a GZIP wrapper on B. Compression
processing is outside scope, so this naming/framing distinction is preserved
as metadata rather than decided by this codec.

D1 Table B-II's fictitious payload has another internal conflict: its Field 2
is labeled ASCII 66 (`B`), but its published octets `A0 78 50 9A 00` decode that
seven-bit field, starting at bit 6, as 98 (`b`). The field-fragment columns imply
second octet `70`, not `78`, for 66. Tests preserve the published bytes with
observed value `b`, and separately encode the stated intended value `B` as
`A0 70 50 9A 00`. Neither choice is silently presented as the other's value.

The application-header construction examples also show both URN and unit name;
their footnotes explicitly acknowledge that this violates exclusivity. D1's
Table B-I further uses version 3, an omitted VMF standard version and illustrative
FAD 15. It is guidance, not a strict positive complete-PDU fixture.

Version-15 field stopping is interpreted above; no unsupported certainty about
an unspecified full-layout variant is intended. Calendar validity beyond the
published component code ranges, and constraints requiring prior messages or
transport state, cannot be established solely by this parser. Future research
must record any further ambiguity here rather than silently invent a rule.

### Original 1995 edition

The [1995 edition][base] remains unsupported. Its Table I (printed pp. 8–10)
begins with the originator-address GPI and contains no version field. Later
editions' Table II designates it as version 0; that designation does not add
a leading version field to the native 1995 format.

Section 5.5.6 and Figure 3 (printed p. 6) specify LSB-first internal packing
with consecutive fields concatenated bit by bit. Section 5.6.6 (printed p. 11)
explicitly requires zero fill in each body's final partial octet. The inspected
1995 text does not explicitly specify header-end padding or the header/body
octet boundary. The later editions' header zero-fill rule cannot establish
what a native 1995 sender did.

[ARL-TR-2448][arl] (April 2001), sections 3.2.4.1–.2 (printed p. 16), describes
FBCB2/INC header processing and cites the original edition as reference 15.
However, section 4.3.3 (printed p. 41) identifies the header as B, and reference
18 (printed p. 46) also dates B to July 1995. These inconsistent references
do not establish verified native 1995 traffic. The reviewed evidence does not
justify a claim that the original edition was never fielded.

Authoritative clarification or independently sourced complete PDUs must
establish the header/body boundary convention before support is reconsidered.
Selectable revision labels in other tools alone do not establish conformance
or supply acceptance-test ground truth.

## Comparison fixtures and acceptance vectors

The comparison schema targets C and D1, not complete B/D support. Its `GRI`
discriminator agrees with the leading recurrence rule; its `urn_unit_name_group`
checks exclusivity. Its authentication-length arithmetic uses `(code+1)*8`
octets, agreeing with 64-bit blocks. Its `future_use_groups` schema accepts
future payloads; that permissiveness is inappropriate for explicit D/D1 strict
profiles. DFDL's calculated output fields are not an API requirement to
backpatch streaming output; VMF uses measurement and replay instead.
More significantly, `milstd2045_application_header.dfdl.xsd` explicitly permits
a VMF message-standard version inside a UMF=1 binary-file header to match
observed traffic. Its own comment acknowledges that Table V marks all such
values illegal. This library rejects that combination; the comparison's
pragmatic interoperability exception is not silently adopted.

The comparison checkout inspected was commit
`b1a489122f2d18bd3a329009d2a8c5f0ba0f0616`.
Fixture audit is per source, not an endorsement of the test directory:

| Comparison fixture | Disposition |
| --- | --- |
| `stringMaxLength`, `stringMaxLengthUnparse`, `stringWSPBug` | Isolated text primitives, not complete PDU vectors |
| `presence1`, `presence2`, `recurrenceField1`, `recurrenceField2` | Control primitives only; no revision conformance claim |
| `test2045msghdr1`, unparsed and operator-indicator variants | Example-derived D1 data; omission of VMF standard-version presence conflicts with mandatory D1 condition; not imported as strict vectors |
| `test2045msghdr2` | SPI fragment, not a complete PDU |
| `test2045msghdr3` | UMF `Undefined_15`; reject as a strict envelope vector |
| `test_2045_C_msghdr1` | C, HeaderSize=24, VMF standard 6017A, FAD2/message1, Flash precedence; intentionally replaced MessageSize with zero. Envelope evidence only; not evidence that an empty body is a valid K02.1 message |
| `test_2045_C_msghdr1_unparsed` | Unparser counterpart of that zero-body example; same limitation |
| `test_2045_C_operator_machine_ind_on_unparsed` | Same C family with acknowledgment indicators enabled; originator address and DTG are present; no body conformance claim |
| `test_2045_C_msghdr2` | Deliberately contains both URN and unit name in originator; TDML expects an exclusivity validation error; negative fixture only |
| `test_2045_D1_minimum_size_header` | Structurally useful six-octet Link16 header; omitted MessageSize requires final-delimited input for full traversal |
| `test_2045_C_minimum_size_header` | Encodes precedence 000, undefined under C Table VII; not a strict positive vector |
| `test_2045_D1_control_release_marking1` | Single 9-bit marking exercise; no body length, so not an independently delimited complete PDU |
| `test_2045_D1_control_release_marking2` | Repeated 9-bit marking exercise; no body length, so not an independently delimited complete PDU |
| `test_2045_C_control_release_marking1` | C text marking exercise but precedence 000 is undefined; not a strict positive vector |
| `test_2045_C_minimum_size_acknoledgement` | Uses undefined C precedence 000 and R/C 000; reject as a strict positive vector |
| `test_2045_D1_all_fields` (`D1_all_fields.dat`) | Response present together with requests/perishability; CANTCO reason present with RC=CANTPRO; SPI0 includes keying material. These violate mandatory conditions; not a positive vector |
| `test_2045_D1_all_fields_x2p` (`.x2.dat`) | Repeats the same all-fields fixture; repetition does not cure those violations |
| `test_2045_D1_all_fields_x2u` | Unparser form of that fixture; generated output is not normative validation |
| `dtTest_01`–`dtTest_07` | Date-time primitive tests only, not envelopes; do not establish revision-specific field conditionality |
| `test_2045_C_msghdr1_invalid_address_group_count` | Deliberately exceeds combined addressee count; negative fixture only |

Local acceptance vectors are literal bytes independently assembled from Table I
using bit offsets, rather than generated by the codec under test. The checked
table in `tests/acceptance/vectors.h` retains historical synthetic UMF=1 binary
and UMF=0 vectors for envelope inspection. Their zero-based layout checks are
B: GRI at 8, UMF at 9, MessageSize value at 30, precedence at 53; C: GRI at 25,
UMF at 26, MessageSize value at 48, precedence at 71; D/D1: GRI at 30, UMF at
31, MessageSize value at 53, precedence at 76.

Current complete-payload tests use independently assembled synthetic UMF=2
frames with FAD1/message1. C/D/D1 explicitly carry raw message-standard code
zero; B carries none. They retain a one-character filename and one-octet test
body, with separate unsized and multiple-descriptor vectors. These synthetic
schemas exercise the native API without asserting assigned MIL-STD-6017
layouts. Binary-file frames remain valid for schema-independent header tests.
The public C fixture in `tests/unit/fixtures.hpp` is the comparison project's
24-octet `test_2045_C_msghdr1` header, including its present zero MessageSize.
Its expected values come from that project's infoset; the codec also reproduces
its bytes. This validates the envelope, not an empty K02.1 message. Upstream's
comment names `K02.1_1.1.VMB` as its modified source; that does not establish a
fielded-capture provenance chain.

C Table XIX's `C8 A0 F0 03 00` is decoded as individual fields: check-fire type 0,
command 1 and target `AB0031`, with the remaining optional fields/groups absent.
D1 Table B-II's published and intended-value variants above are likewise decoded
as fields. Their caller-authored schemas transcribe public examples; they are
not complete MIL-STD-6017 message definitions. Synthetic VMF envelopes provide
complete-PDU framing for these payload tests, using the explicit edition
selection and message identity required by the native schema API.

The codec has been tested as interoperable with this DFDL definition using
Apache Daffodil 4.2.0 before the native schema API revision: upstream C
parser/unparser cases and synthetic C/D1 complete PDUs passed in both directions. The checks treated length-bounded
bodies as binary data and did not validate tactical meaning, signatures or a
6017 message set. See the [interoperability record](testing.md#interoperability-record)
for the tested scope and source revision.

[assist]: https://quicksearch.dla.mil/qsDocDetails.aspx?ident_number=117743
[base]: https://quicksearch.dla.mil/WMX/Default.aspx?token=14874
[a]: https://quicksearch.dla.mil/WMX/Default.aspx?token=261654
[arl]: https://www.govinfo.gov/content/pkg/GOVPUB-D101-PURL-gpo2611/pdf/GOVPUB-D101-PURL-gpo2611.pdf
[dfdl]: https://github.com/DFDLSchemas/mil-std-2045
[b]: https://github.com/DFDLSchemas/mil-std-2045/blob/master/doc/MIL-STD-2045_47001B.pdf
[c]: https://github.com/DFDLSchemas/mil-std-2045/blob/master/doc/MIL-STD-2045_47001C.pdf
[d1]: https://github.com/DFDLSchemas/mil-std-2045/blob/master/doc/MIL-STD-2045_47001D_CHANGE-1.pdf
