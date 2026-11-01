# COMPLY.1: Turkey's asset freezes imported from MASAK's table

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-01. Row: [COMPLY.1](../ROADMAP.md#slice-comply.1) (track C, batch 31; the
[parallel tracks](2026-10-31-parallel-tracks.md)). Contract: the registers'
[sanctions and embargo screening](../design/registers.md#screening). The previous batch wrote the UN embargo
([DPRK and Iran](2026-11-01-comply-1-un-dprk-and-iran.md)) and named Turkey's asset-freeze decisions and their importer
as the next work.

## The user's decision

The user's words (2026-11-01, about 01:25 local time): "approve the downloads we need to do everything properly, I will
contect a lawyer later! DO NOT BLOCK ANYTHING CONTACTING A LAWYER IS ... HARD! I WANT NETWORK PART TO BE COMPLETE AND
PROPERLY DONE AND GOLD STANDARD AND ELITE". The decision approves COMPLY.1's official sanctions lists, Turkey's
asset-freeze decisions among them: MASAK's answers below were read under it, from MASAK's own host only.

## CI first

No failure comes from a track C change. The complete run dispatched for the UN embargo (41298463221, 13453acb) and push
run 41298450436 were still queued at 10:41 UTC. Completed jobs of the open runs, all owned elsewhere:

- **Push run 41286750175 (8311a54e).** win-clang-cl and win-clang-cl-shipping: lld-link's undefined `__udivti3` and
  `__divti3` from the 128-bit division in `tests/foundation/perf/test_instruction_access.cpp` (HW-RESILIENCE.2, track A;
  the file is unchanged on main since 76e61d51, so still open). win-public-checks: track B's last-write-time case, fixed
  by cc5ee9e9, which 8311a54e predates. linux-gcc-arm64-asan: AddressSanitizer stack-buffer-overflow (a read of size 8)
  in Shewchuk's reference `fast_expansion_sum_zeroelim`
  (`tests/geometry/geometry-primitives/reference/shewchuk-predicates.c:1064`, through `orient2dexact` at line 1485) fails the three "agrees with Shewchuk's adaptive and exact" cases; the reference
  and its test came with 02c0e664 and e519b77d (the geometry predicates' owner).
- **Complete run 41274380605 (de5e10b0).** win-debug, win-release, win-shipping, win-shipping-profile and
  win-public-checks fail the same last-write-time case (fixed by cc5ee9e9); win-release also fails "diagnostic host:
  the application's wild write leaves one attributable crash capture, and the repaired one runs clean", track A's,
  reported by batch 30.

<a id="masak"></a>
## MASAK's table

The register's pending reason said MASAK publishes the decisions "through a web application with no list file at a
stable address". That is true of a file, but the application has a machine route: its bundle
(`masak.hmb.gov.tr/static/js/main.*.chunk.js`) reads four routes under `portal/v2/masak/mvd/`:

- `yaptirim`, `orgut` and `uyruk` (GET): the sanction types, organisations and nationalities the search offers.
- `malvarligi-dondurulanlar` (POST, JSON `{"ad", "uyruk", "orgut", "yaptirim"}`): the search, which the form refuses
  with every field empty. A GET is refused by the host ("İstek Engellendi").

The four sanction types are:

- "BMGK Yaptırımları Kapsamında (6415 sayılı kanun 5. Maddesine istinaden)": the UN's lists, Law No. 6415, Article 5.
- "Yabancı Ülke talebi (6415 sayılı kanun 6. Maddesine istinaden)": foreign requests, Article 6.
- "İç Dondurma (6415 sayılı kanun 7. Maddesine istinaden)": domestic freezes, Article 7.
- "7262 Sayılı Kanun Kapsamında": Law No. 7262.

Searching by each type returns 476, 109, 1 442 and 277 rows. Their `sira_no` are unique across the four and run from 1
to 2 304 = 476 + 109 + 1 442 + 277, so the four answers are the whole numbered table. Two answers to the same search
differ only in `elapsedTime` (the server's milliseconds), and the rows come in the same order.

Each row has 13 text columns:

| Column | Meaning |
|---|---|
| `ad_soyad_unvan` | the name or title |
| `tckn_vkn` | "TCKN/VKN/GKN PASAPORT NO" |
| `uyrugu`, `diger_uyruk` | nationality and other nationalities |
| `yaptirim_turu` | the sanction type |
| `anne_adı`, `baba_adı` | the mother's and father's names |
| `dogum_tarihi`, `dogum_yeri` | the birth date and place |
| `orgutu` | the organisation |
| `karar_sayisi` | the Official Gazette day and issue |
| `diger_adlari` | other names |

None of them is a kind.

## What landed

- **The register.** Four lists replace the two pending reasons, all on
  `https://masak.hmb.gov.tr/portal/v2/masak/mvd/malvarligi-dondurulanlar`:

  | List | Serves | Posts the type |
  |---|---|---|
  | `tr.masak.un` | `tr.asset-freeze` | Article 5 |
  | `tr.masak.foreign` | `tr.asset-freeze` | Article 6 |
  | `tr.masak.domestic` | `tr.asset-freeze` | Article 7 |
  | `tr.masak.7262` | `tr.7262.proliferation-freeze` | Law No. 7262 |

  The lists' new key `post` is the search a list posts: null for a file, an object of texts for an application's
  answer. `check-registers.py` requires it from a format that posts and refuses it from a file. `hmb.gov.tr` (the
  Ministry of Treasury and Finance, MASAK's ministry) joins TR's official hosts. Each list's maximum age is 31 days.
  The four answers read on 2026-11-01 were recorded:

  | List | Bytes | SHA-256 (canonical) |
  |---|---|---|
  | `tr.masak.un` | 296 936 | `2176c7ef…57d95a` |
  | `tr.masak.foreign` | 52 410 | `a28c07dc…600e36` |
  | `tr.masak.domestic` | 683 899 | `a800895e…93f7` |
  | `tr.masak.7262` | 120 032 | `3889c1a6…49772` |

- **The fetch** ([import-sanctions.py](../../scripts/import-sanctions.py)). A format declares its file suffix, its
  reader, its canonical form, its census and whether it posts.
  - The fetch posts the list's search as JSON (`register-review.py`'s `download` gained the body) and keeps the answer
    in canonical form: its envelope checked, `elapsedTime` dropped, keys sorted. An unchanged table is the same bytes:
    the cloud machine's fetch, 17 minutes later and from another network, matched all four recorded digests.
  - The fetch record holds the `instant` read. MASAK's answer carries no stamp, so that instant is its publication.
  - A file published again later, with the same bytes and a later instant, is the next generation. A list read again
    stays fresh instead of ageing on the earlier reading.
  - Before any list, the fetch reads the application's sanction types. It refuses a type no list posts, and a listed
    type the application no longer offers.
- **The reader** ([the contract](../design/registers.md#screening) states each rule). It reads the envelope exactly: a
  status of 200 "OK", no message, errors or result, the 13 columns in order, the row count, every row's columns and
  types, and every row of the type posted.
  - **Kind.** A party is an individual when its row gives a birth year or a parent's name, a vessel when it has an IMO
    number, else of unknown kind. The organisation column cannot decide it: three persons with birth dates stand under
    "TÜZEL KİŞİ" (legal person).
  - **Births.** The table writes `7/2/1966` beside `4/13/1978` and `23/4/1966`, so a numeric date that reads both ways
    records both days. Turkish and English month names, spans of years and alternatives are read. A field read only in
    part records no period: a name, a place, a Hijri date or a typo. So does an approximate field ("Tahmini olarak",
    "Yaklaşık", "Muhtemelen").
  - **Identifiers.** A number after a word that names its kind takes that kind: "pasaport", "ulusal kimlik", "TCKN:",
    "VKN:", "YKN:", "IMO number:", "SWIFT/BIC:", "Email:". Any other number is typed by its check digits:
    - a TCKN or GKN is `national-id.tr` (1 335 of the table's 1 345 eleven-digit numbers pass);
    - a VKN is `tax-id.tr` (191 of 201 ten-digit numbers pass);
    - a TCKN and a VKN typed as one 21-digit number are both (all 12 such numbers split);
    - a Turkish number's check digits outrank its word;
    - any other number is a passport.

    A 9-digit number whose zero-padded form passes the VKN check is also that VKN. The domestic list has 14 nine-digit
    numbers and 11 of them pass, where chance gives one in ten. Telephone, fax and prison-register numbers, dates and
    notes are never compared.
  - **Names.** Aliases split at commas, semicolons and line ends, and at enumerations: `a) b) c)` in order (also glued
    to a name, as in "Thorjqb) Rusdjanc)") and `1- 2- 3-`. The labels "ORJİNAL YAZIM:" and "ESKİ ADI:" split too. A
    parenthesis is an alias, except a note ("DAHA ÖNCE LİSTELENEN İSİM", "İSMİNİN İLK HARFLERİ") or a country. A
    legal form after a comma stays with its name ("CO., LTD").
  - **Countries.** A Turkish country table on the existing codes maps each name; an unknown name refuses the import.
    The Russian federal subjects the table prints map to RU. "USSR" and "Oran" (an Algerian city printed beside
    Algeria) record no country.
  - **Listing day and programmes.** The Official Gazette day is the listing day; a decision with only its issue
    ("/ 32153") has none. The sanction type and the organisation are the programmes.
- **Unknown kind in crd-comply** ([sanctions.hpp](../../engine/foundation/comply/include/crd/comply/sanctions.hpp)).
  `PartyKind::Unknown` is a list party's kind when the list names none, and no subject's kind contradicts it. Until now
  every party needed a kind, and a wrong guess would have discounted the matching hit to a Clear verdict. A subject's
  kind is never unknown: `screen` refuses one (`kind`) and `parse_party_kind` reads only the four subject kinds, so
  `ceridc screen --kind unknown` is refused. A birth on an unknown party is refused, as on any party that is not an
  individual.
- **Approximate births, a defect found and fixed.** An approximate birth (OFAC's circa, the EU's circa, the UN's
  APPROXIMATELY) recorded no period, but the party's exact births beside it stayed. A subject born near the
  approximate year was then discounted by the exact years, and the verdict was Clear. An approximate birth now leaves
  the party no period at all. In the files read on 2026-11-01 this changes 59 parties: 4 of OFAC's SDN, 27 of the EU's
  and 28 of the UN's. A diff of the full imports before and after shows nothing else changed.

The full table imports into 2 304 parties, in 1 653 rows whose birth field is read and 73 whose field is read only in
part:

| List | Individual | Unknown | Vessel |
|---|---|---|---|
| `tr.masak.un` | 236 | 240 | 0 |
| `tr.masak.foreign` | 60 | 49 | 0 |
| `tr.masak.domestic` | 1 356 | 86 | 0 |
| `tr.masak.7262` | 72 | 192 | 13 |

## Verification

- **Python.** 7 new cases in `SanctionsLists`:
  - a real-byte import of the four excerpts;
  - births, identifiers and check digits;
  - 17 refusals of the reader;
  - a fetch against a fake application: it posts, keeps the canonical answer and refuses an added or a withdrawn type;
  - the next generation for a later read;
  - approximate births;
  - the guard's `post` rule.

  The 23 existing cases were adapted to the new signatures and APPROXIMATE. The class has 30 cases, all passing. 15
  importer and guard mutations are each caught; the TCKN check digits' mutation was caught only after a case with a
  wrong tenth digit was added. In the whole file, 302 cases ran: 298 passed, 3 were skipped and
  `test_generated_target_folders` failed because it needs Ninja, which this PC does not have under the no-builds rule.
- **The cloud machine.** On `cerid-track-c`, linux-gcc-debug, c4967cf2 and c66ca5f0: 0 warnings.
  - Fuzzing: the fuzz harness has a fourth party of unknown kind, its model never discounts it by kind, and it has
    three new seeds. The corpus was regenerated to the harness's 31 seeds and replayed clean.
  - CTest: 41 comply, guard and check tests passed: the import of all nine authorities, the hidden `[.imported]` cases
    (8 cases, 1 360 assertions, the new Turkish case among them) and the fuzz corpus.
  - The ceridc `[screen]` and `[comply]` cases (11, 177 assertions) passed on the real binary.
  - Strict LLVM 20 analysis of the changed C++ is clean. It found one issue on the first pass, an rvalue parameter
    never moved, which was fixed with a constructor that takes the lists.
  - Seven C++ and importer mutations were each caught:
    - three on the unknown kind (contradicted by every kind; a subject of unknown kind accepted; "unknown" parsed as a
      subject's kind, caught by the ceridc case);
    - the lost VKN zero, a numeric date read one way, and every MASAK party an individual, each by the hidden case.
  - The machine fetched the four answers itself (the digests matched) and imported them. `ceridc screen` against the
    full lists:
    - a listed Turkish citizen's TCKN: blocked;
    - "Abdallah Azzam Brigades" as an entity: review;
    - an unlisted name: clear.

  The machine ran from about 11:14 to 11:29 UTC (about 0.25 h) and is stopped.

## Next

1. **CI.** Audit complete run 41298463221 (13453acb) when it ends. Then audit the first hosted run that includes this
   batch: the first Windows, clang-cl and ARM64 compile of `PartyKind::Unknown`, and the first Windows run of the MASAK
   import and of the 7 new Python cases.
2. **COMPLY.1.** Three items remain:
   - Read the Darfur renewal before 11 December 2026.
   - Read the Wassenaar Category 5 Part 2 and Turkey's dual-use communiqué for the classifications' bases.
   - Cross-check MASAK's table against the Official Gazette: read the latest decisions of each type
     (2026-04-04, 2026-07-14, 2024-09-09, 2025-10-02) from resmigazete.gov.tr and confirm each named party is in the
     table.
