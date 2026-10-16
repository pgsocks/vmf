# Local conformance requirements

This matrix catalogs requirements decidable from one complete header, its
bounded body and its configured schema. Read it with the ordered grammar,
revision code tables and source-conflict register in [format.md](format.md).
It is a codec contract, not a claim of implementing every service required of
a MIL-STD-2045 system. Presence requirements apply within their containing
present group; an absent parent supplies no child indicators.

Sources below refer to B (1998), C (2002), D (2005) and D with Change 1 (2008).
“Common” means all four supported editions. Section numbers refer to each
edition unless a prefix limits them. Each row is an independently enforceable
check; grammar entries additionally specify exact field widths and positions.

| ID | Editions | Local requirement | Primary source |
| --- | --- | --- | --- |
| W01 | Common | Concatenate header fields in Table I order without implicit internal alignment. | Table I; 5.3, 5.5.6 |
| W02 | Common | Pack header integers and ASCII units least-significant bit first into octet bits 0 through 7. | 5.3; Appendix B construction explanation |
| W03 | Common | Fill unused final header-octet bits with zero. | 5.3, 5.5.6; Appendix B |
| W04 | Common | Fill each body's unused final-octet bits with zero independently. | 5.6.9; Appendix B |
| W05 | Common | An absent field/group omits its content and all nested indicators. | 5.5.1, 5.5.3 |
| W06 | Common | A repeated occurrence has a leading recurrence flag describing whether another occurrence follows; the final occurrence has flag zero. | 5.5.2, 5.5.4 |
| W07 | Common | Header text uses seven-bit ASCII, with DELETE below its maximum or no terminator at its maximum. | 5.5.5; 5.6.3.2, 5.6.8, 5.6.25 |
| W08 | Common | Do not exceed any Table I field/group occurrence limit. | Table I |
| V01 | Common | Ordinary header version must match the selected B/C/D/D1 profile. | Table II; explicit-profile API policy |
| V02 | C/D/D1 | Version 15 has no compression and requires originator and recipient groups. | 5.6.1; D/D1 5.7.2.5.2; C 5.7.4.2 |
| V03 | D/D1 | All fifteen future-use indicators are absent for versions 3 and 4. | 5.5.6.4; 5.7.2.1.9 |
| V04 | Common | Validate compression codes against Table III; absence means no compression. | 5.6.2, Table III |
| A01 | Common | Every present address has exactly one of URN and unit name. | 5.7.2.2.2–.5 |
| A02 | Common | Recipient and information-addressee counts sum to at most sixteen. | 5.6.3; Table I |
| A03 | B | An absent recipient group requires the information group absent. | 5.7.2.2.6 |
| A04 | C/D/D1 | A destination URN occurs at most once across recipients and information addressees. | C 5.7.4.13; D/D1 5.7.2.5.13 |
| A06 | Common | An absent originator group requires the entire request group absent. | 5.7.2.2.1 |
| M01 | Common | Validate UMF against the selected edition's Table IV. | 5.6.4, Table IV |
| M02 | C/D/D1 | Validate each present message-standard version jointly with UMF and envelope edition. | 5.6.4.7, Table V |
| M03 | Common | VMF identification is present for VMF; identification is absent for the explicitly excluded UMFs. | 5.6.4; Appendix A; cases in 5.7.2.1 |
| M04 | C/D/D1 | XML-VMF requires identification; XML-MTF excludes it. | 5.6.4.5/.6; D/D1 cases 3, 8 |
| M05 | D/D1 | VMF requires a message-standard version. | 5.7.2.2.18 |
| M06 | Common | Subtype is absent when identification is absent. | Table I |
| M07 | Common | Message number is 1 through 127. | 5.6.6 |
| M08 | Common | Validate VMF FAD against the edition's assignments. | B Table A-1; C Table XIV; D/D1 Table A-I |
| M09 | Common | Binary-file transfer supplies filename and message size and excludes identification. | 5.6.4.1 |
| M10 | Common | An ordinary descriptor has no Response and carries a body. | 5.7.2.1.1 |
| M11 | Common | An acknowledgment descriptor has Response present and no body. | 5.7.2.1.2 |
| M12 | Common | An acknowledgment descriptor has no perishability group. | 5.7.2.1.2 |
| M13 | Common | An acknowledgment descriptor has no acknowledgment-request group. | 5.7.2.1.2 |
| M14 | Common | An acknowledgment header contains originator and recipient address groups. | B/C 5.7.5.2; D/D1 5.7.4.1 |
| M15 | Common | Compression applies to data-bearing descriptors, not acknowledgments. | 5.7.2.1.5 |
| L01 | Common | Repeated handling descriptors require their MessageSize fields. | 5.6.9; 5.7.2.2.7 |
| L02 | C/D/D1 | Repeated handling descriptors require HeaderSize. | 5.7.2.2.7 |
| L03 | Common | MessageSize counts associated body octets, including final padding; compressed data sizes count compressed octets. | 5.6.2, 5.6.9 |
| L04 | C/D/D1 | Present HeaderSize equals the padded header's octet size, including HeaderSize itself. | 5.6.27 |
| L05 | Common | Bodies appear in handling-descriptor order with their respective sizes. | B 5.7.5.5; C 5.7.4.6; D/D1 5.7.2.5.6 |
| L06 | B | The entire concatenated block including its header is at most 1,048,575 octets. | 5.7.5.5 |
| L07 | C | Apply the documented conservative whole-PDU bound of 1,048,575 octets. | Conflict/policy: 5.7.4.6, 5.8.1 |
| L08 | D/D1 | Each individual body is at most 1,048,575 octets. | 5.7.2.5.6 |
| C01 | Common | Validate operation codes against the edition; B excludes code 3. | 5.6.10; B Table V, later Table VI |
| C02 | Common | Validate precedence using B/C or D/D1 assignments. | 5.6.12; B Table VI, later Table VII |
| C03 | D/D1 | DOI-103 requires precedence code 5. | 5.7.2.2.16 |
| C04 | Common | Classification is a two-bit code in 0 through 3. | 5.6.13 |
| C05 | B | A present control/release marking is exactly two seven-bit ASCII units. | 5.6.14 |
| C06 | C | Control/release marking text has maximum 32 ASCII units. | 5.6.14 |
| C07 | D/D1 | Control/release markings repeat at most sixteen nine-bit codes. | 5.6.14 |
| D01 | Common | Year code is 0 through 99, with the documented 1995–2094 mapping. | B Table VIII; C/D/D1 Table IX |
| D02 | Common | Month code is 1 through 12. | DTG table |
| D03 | Common | Day code is 1 through 31. | DTG table |
| D04 | Common | Hour code is 0 through 23. | DTG table |
| D05 | Common | Minute code is 0 through 59. | DTG table |
| D06 | B/C | Second code is 0 through 59. | B Table VIII; C Table IX |
| D07 | D/D1 | Second code is 0 through 59 or 63 (“no statement”). | Table IX |
| D08 | Common | Present DTG extension fits twelve bits; perishability has no extension field. | Table I; 5.6.16/.17 |
| D09 | Common | Any set machine/operator/reply request bit requires originator DTG. | 5.7.2.2.11 |
| D10 | D/D1 | Retransmission requires originator DTG. | 5.7.2.2.17 |
| R01 | Common | Receipt/compliance code is 1 through 6. | 5.6.22; B Table IX, later Table X |
| R02 | Common | CANTCO reason is absent unless receipt/compliance is 6. | 5.7.2.2.8 |
| R03 | Common | CANTPRO reason is absent unless receipt/compliance is 2. | 5.7.2.2.9 |
| R04 | B | Amplification is absent unless receipt/compliance is 4, 5 or 6. | 5.7.2.2.10 |
| R05 | Common | VMF reason codes use the selected edition's Appendix A tables. | B Tables A-2/A-3; C Tables XVI/XVII; D/D1 Tables A-III/A-IV |
| R06 | Common | References number at most four; B references additionally contain FAD and message number. | Table I; 5.6.26 |
| S01 | C/D/D1 | Derive security-span lengths from their defined direct/zero-based length codes without inserting byte alignment. | 5.6.29–.41; Table I |
| S02 | C/D/D1 | Token lengths are shared and token count is at most seventeen. | Table I; 5.6.33/.34 |
| S03 | C/D/D1 | SPI 0 requires Authentication A. | 5.7.2.2.13 |
| S04 | C/D/D1 | SPI 0 excludes keying material. | 5.7.2.2.13 |
| S05 | C/D/D1 | SPI 0 excludes initialization material. | 5.7.2.2.13 |
| S06 | C/D/D1 | SPI 0 excludes tokens. | 5.7.2.2.13 |
| S07 | C/D/D1 | SPI 0 excludes security padding. | 5.7.2.2.13 |
| S08 | C/D/D1 | SPI 0's DSA signature is forty octets (length code 4). | Table XI; Appendix D |
| S09 | C/D/D1 | Authentication B requires signed-acknowledgment form and Authentication A. | 5.7.2.1.7 |
| S10 | C/D/D1 | Signed acknowledgment has no signed acknowledgment request. | 5.7.2.1.7 |
| S11 | C/D/D1 | An absent request group requires signed acknowledgment request zero. | 5.7.2.2.14 |
| S12 | C/D/D1 | Signed acknowledgment request requires the request group present. | 5.7.2.2.15 |

Native payload schemas select VMF (UMF 2) and one explicit registry profile.
The core shares defaults and validation with the optional `vmf-schema/1` JSON
adapter. Profiles do not infer a message-standard edition. Exact resolved
identities distinguish absent values from present zero, and duplicate wire
identities fail. A codec profile must agree with its registry.

Ordered indexes and typed membership paths must establish contiguous,
unambiguous scopes. Explicit FPI/GPI and FRI/GRI controls consume each control
bit once; membership and one-bit width alone do not imply an indicator.
Ordinary unsigned data defaults to widths 1–64 with independent LSB packing
and significance. Signed representation, variable length, text modes and finite
repeat bounds are explicit. Constants constrain supplied values rather than
providing defaults. These are authoring rules and do not assert a complete
MIL-STD-6017 message implementation.

The codec also enforces the API's limits, exact schema identity, integer widths,
dependency scope, event grammar and streaming ownership contracts. Those are
library requirements rather than additional MIL-STD wire rules. Unknown SPI
metadata is inspectable; unknown SPI body processing and compression fail
explicitly. SPI 0 body traversal does not imply signature verification.

The following requirements cannot be proved from one local PDU: request/response
matching; original/responder address reversal; copied acknowledgment UMF and
identification; originator-DTG uniqueness against prior messages; required
extension equality with a prior message; expiration relative to a clock;
retransmission timers; broadcast network delivery and suppressed broadcast replies; transport-dependent size
presence; reliable reply selection; segmentation/reassembly; and cryptographic
verification. The caller supplies those services and external MIL-STD-6017
assignments, cases, subtype meanings and release-country meanings. Calendar
code ranges are specified above; the standard does not supply a complete
Gregorian calendar-validation algorithm here.

The source register deliberately preserves unresolved interpretation questions:
the unsupported 1995 edition's header-end padding and header/body boundary;
version-15 stopping; C maximum-size wording; example header violations; and
D1 Table B-II's contradictory ASCII/octet values. Internal bit concatenation
and body zero fill are specified by the 1995 edition.
The comparison schema's observed-traffic exception for UMF=1 with a message
standard version is not silently accepted by this strict profile contract.
