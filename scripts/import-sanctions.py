#!/usr/bin/env python3
"""Fetch the official sanctions lists and import each into a cerid-sanctions/1 generation (COMPLY.1).

The legal register's `sanctions_lists` (cmake/legal-register.json, docs/design/registers.md#screening) names every
authority's official file: the register entry it serves, the canonical URL, the hosts that URL may redirect to, the
importer's format, the list's maximum age and the record of the file read (day, publication, bytes, SHA-256,
parties). The raw files stay outside the repository: they are re-fetched from the authority and verified by digest.

  python scripts/import-sanctions.py fetch --into DIR [--authority A ...]
      download each official file to DIR/<authority>.xml (.json for an application's answer, posted its `post` and
      kept in canonical form) with DIR/<authority>.fetch.json (URL, day, instant, bytes, SHA-256) and say whether it
      is the file the register recorded or a newer publication.
  python scripts/import-sanctions.py import --from DIR --out DIR [--authority A ...]
      write OUT/<authority>.sanctions.toml from the fetched file (ceridc screen --lists OUT reads it). The generation
      is the publication instant as YYYYMMDDhhmmss; a list stamped only with its day takes YYYYMMDD000000 and counts
      up from the previous generation in OUT for a second publication that day. A file already imported is left as
      it is, and an older publication than OUT's is refused. The embargoed destinations authored in the register's
      `sanctions_lists.embargoes` are written beside them (no fetch: the regulations they were read from are pinned
      by the register entry they cite), each generation dated by the day its text was read.
  python scripts/import-sanctions.py record --from DIR [--authority A ...]
      record the fetched file in the register as the list read (the first read, then each reviewed refresh).

Every reader fails closed: an element, identifier type, country name or date it does not know refuses the import
with the party and the value, so a change in an authority's format is read, never guessed. Turkey's MASAK table is
typed by hand: its free-text births and identifiers are read only as far as each value is understood, a birth not
read whole recording no period (so it never discounts a hit) and a fragment that identifies nothing recording no
identifier (see the MASAK section).
"""
import argparse
import datetime
import hashlib
import http.client
import importlib.util
import json
import re
import sys
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
import xml.parsers.expat
from pathlib import Path
from typing import Callable, NamedTuple, Optional

ROOT = Path(__file__).resolve().parents[1]
REGISTER = 'cmake/legal-register.json'
TOOL = 'scripts/import-sanctions.py'
FORMAT = 'cerid-sanctions/1'
SUFFIX = '.sanctions.toml'
FETCH_SCHEMA = 'cerid-sanctions-fetch/1'
# The decoder's bounds (crd/comply/sanctions.hpp and generation.hpp; a Python case holds them equal), so a file the
# decoder would refuse is refused here, naming the party.
FILE_LIMIT = 64 * 1024 * 1024  # kMaxSanctionsBytes: an official file, and one generation
MAX_PARTIES = 131072  # kMaxParties
MAX_NAMES = 512  # kMaxPartyNames
MAX_ITEMS = 64  # kMaxPartyItems: programmes, births, countries
MAX_IDENTIFIERS = 1024  # kMaxPartyIdentifiers
MAX_TEXT = 4096  # kMaxTextBytes
AUTHORITY = re.compile(r'^[a-z0-9]+(?:[.-][a-z0-9]+)*$')

_spec = importlib.util.spec_from_file_location('gen_comply_rules', Path(__file__).resolve().parent /
                                               'gen-comply-rules.py')
_rules = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_rules)


class SanctionsError(Exception):
    pass


class OpenReading(SanctionsError):
    """An authored embargo whose reading still owes texts: never written, so a screening naming its authority finds
    the list missing and fails closed instead of clearing a place the texts not yet read may restrict."""


# Country names as the lists print them (OFAC, the UN and the UK; the EU prints ISO codes), case-insensitive, to
# ISO 3166-1 alpha-2. None marks a name that is no single current country (a former state, a disputed or
# multinational region, a flag the authority calls false): it is read and records no country.
COUNTRIES = {
    'AE': ['United Arab Emirates'], 'AF': ['Afghanistan'], 'AG': ['Antigua and Barbuda', 'Antigua & Barbuda'],
    'AL': ['Albania'], 'AM': ['Armenia'], 'AO': ['Angola'], 'AR': ['Argentina'], 'AT': ['Austria'],
    'AU': ['Australia'], 'AW': ['Aruba'], 'AZ': ['Azerbaijan'], 'BA': ['Bosnia and Herzegovina'],
    'BB': ['Barbados'], 'BD': ['Bangladesh'], 'BE': ['Belgium'], 'BF': ['Burkina Faso'], 'BG': ['Bulgaria'],
    'BH': ['Bahrain'], 'BJ': ['Benin'], 'BM': ['Bermuda'], 'BO': ['Bolivia'], 'BR': ['Brazil'],
    'BS': ['Bahamas', 'Bahamas, The'], 'BY': ['Belarus'], 'BZ': ['Belize'], 'CA': ['Canada'],
    'CD': ['Congo, Democratic Republic of the', 'Democratic Republic of the Congo', 'Congo (Democratic Republic)'],
    'CF': ['Central African Republic'], 'CG': ['Congo, Republic of the', 'Congo'], 'CH': ['Switzerland'],
    'CI': ['Cote d Ivoire'], 'CK': ['Cook Islands'], 'CL': ['Chile'], 'CM': ['Cameroon'], 'CN': ['China'],
    'CO': ['Colombia'], 'CR': ['Costa Rica'], 'CU': ['Cuba'], 'CV': ['Cabo Verde', 'Cape Verde'], 'CW': ['Curacao'],
    'CY': ['Cyprus'], 'CZ': ['Czech Republic', 'Czechia'], 'DE': ['Germany'], 'DJ': ['Djibouti'], 'DK': ['Denmark'],
    'DM': ['Dominica'], 'DO': ['Dominican Republic'], 'DZ': ['Algeria'], 'EC': ['Ecuador'], 'EE': ['Estonia'],
    'EG': ['Egypt'], 'ER': ['Eritrea'], 'ES': ['Spain'], 'ET': ['Ethiopia'], 'FI': ['Finland'], 'FJ': ['Fiji'],
    'FR': ['France'], 'GA': ['Gabon'], 'GB': ['United Kingdom', 'United Kingdom of Great Britain and Northern Ireland'],
    'GE': ['Georgia'], 'GG': ['Guernsey'], 'GH': ['Ghana'], 'GI': ['Gibraltar'], 'GM': ['Gambia', 'The Gambia'],
    'GN': ['Guinea'], 'GQ': ['Equatorial Guinea'], 'GR': ['Greece'], 'GT': ['Guatemala'], 'GW': ['Guinea-Bissau'],
    'GY': ['Guyana'], 'HK': ['Hong Kong', 'China, Hong Kong Special Administrative Region'], 'HN': ['Honduras'],
    'HR': ['Croatia'], 'HT': ['Haiti'], 'HU': ['Hungary'], 'ID': ['Indonesia'], 'IE': ['Ireland'], 'IL': ['Israel'],
    'IM': ['Man, Isle of'], 'IN': ['India'], 'IQ': ['Iraq'], 'IR': ['Iran', 'Iran (Islamic Republic of)',
                                                                       'Region: Iran'],
    'IS': ['Iceland'], 'IT': ['Italy'], 'JE': ['Jersey'], 'JM': ['Jamaica'], 'JO': ['Jordan'], 'JP': ['Japan'],
    'KE': ['Kenya'], 'KG': ['Kyrgyzstan'], 'KH': ['Cambodia'], 'KI': ['Kiribati'], 'KM': ['Comoros'],
    'KN': ['Saint Kitts and Nevis', 'St Kitts & Nevis', 'St Kitts and Nevis', 'St. Kitts & Nevis',
           'St. Kitts and Nevis'],
    'KP': ['Korea, North', "Democratic People's Republic of Korea", 'DPRK', 'North Korea'],
    'KR': ['Korea, South', 'South Korea'], 'KW': ['Kuwait'], 'KY': ['Cayman Islands'], 'KZ': ['Kazakhstan'],
    'LA': ['Laos'], 'LB': ['Lebanon'], 'LC': ['Saint Lucia', 'St Lucia'], 'LI': ['Liechtenstein'],
    'LK': ['Sri Lanka'], 'LR': ['Liberia'], 'LT': ['Lithuania'], 'LU': ['Luxembourg'], 'LV': ['Latvia'],
    'LY': ['Libya'], 'MA': ['Morocco'], 'MC': ['Monaco'], 'MD': ['Moldova'], 'ME': ['Montenegro'],
    'MH': ['Marshall Islands'], 'MK': ['North Macedonia, The Republic of'], 'ML': ['Mali', 'Region: Northern Mali'],
    'MM': ['Burma', 'Myanmar'], 'MN': ['Mongolia'], 'MO': ['Macau'], 'MR': ['Mauritania'], 'MT': ['Malta'],
    'MU': ['Mauritius'], 'MV': ['Maldives'], 'MX': ['Mexico'], 'MY': ['Malaysia'], 'MZ': ['Mozambique'],
    'NA': ['Namibia'], 'NE': ['Niger'], 'NG': ['Nigeria'], 'NI': ['Nicaragua'], 'NL': ['Netherlands'],
    'NO': ['Norway'], 'NZ': ['New Zealand'], 'OM': ['Oman'], 'PA': ['Panama'], 'PE': ['Peru'],
    'PH': ['Philippines'], 'PK': ['Pakistan'], 'PL': ['Poland'],
    'PS': ['Palestinian', 'State of Palestine', 'Occupied Palestinian Territories', 'West Bank', 'Region: Gaza',
           'Region: North Gaza', 'Region: Northern Gaza', 'Region: West Bank'],
    'PT': ['Portugal'], 'PW': ['Palau', 'Republic of Palau'], 'PY': ['Paraguay'], 'QA': ['Qatar'],
    'RO': ['Romania'], 'RS': ['Serbia'], 'RU': ['Russia', 'Russian Federation', 'Region: Russia'], 'RW': ['Rwanda'],
    'SA': ['Saudi Arabia'], 'SC': ['Seychelles'], 'SD': ['Sudan'], 'SE': ['Sweden'], 'SG': ['Singapore'],
    'SI': ['Slovenia'], 'SK': ['Slovakia'], 'SL': ['Sierra Leone'], 'SM': ['San Marino'], 'SN': ['Senegal'],
    'SO': ['Somalia'], 'SR': ['Suriname'], 'SS': ['South Sudan'],
    'ST': ['Sao Tome and Principe', 'Sao Tome & Principe'], 'SV': ['El Salvador'], 'SX': ['Sint Maarten'],
    'SY': ['Syria', 'Syrian Arab Republic'], 'SZ': ['Eswatini'], 'TD': ['Chad'], 'TG': ['Togo'],
    'TH': ['Thailand'], 'TJ': ['Tajikistan'], 'TM': ['Turkmenistan'], 'TN': ['Tunisia'],
    'TR': ['Turkey', 'Türkiye'], 'TT': ['Trinidad and Tobago'], 'TV': ['Tuvalu'], 'TW': ['Taiwan'],
    'TZ': ['Tanzania', 'United Republic of Tanzania'], 'UA': ['Ukraine', 'Region: Crimea'], 'UG': ['Uganda'],
    'US': ['United States', 'United States of America'], 'UY': ['Uruguay'], 'UZ': ['Uzbekistan'],
    'VC': ['Saint Vincent and the Grenadines', 'St. Vincent and Grenadines', 'St. Vincent and the Grenadines',
           'St Vincent'],
    'VE': ['Venezuela'], 'VG': ['Virgin Islands, British', 'British Virgin Islands'], 'VN': ['Vietnam', 'Viet Nam'],
    'VU': ['Vanuatu'], 'WS': ['Samoa'], 'XK': ['Kosovo'], 'YE': ['Yemen'], 'ZA': ['South Africa'],
    'ZM': ['Zambia'], 'ZW': ['Zimbabwe'],
    None: ['former Soviet Union', 'Region: Commonwealth of Independent States', 'Region: Jammu and Kashmir',
           'Region: Kashmir', 'Region: Kafia Kingi', 'Botswana False', 'Mozambique (False)', 'None Identified',
           'Unknown', 'na'],
}
COUNTRY_NAMES = {name.casefold(): code for code, names in COUNTRIES.items() for name in names}
# ISO codes the EU file prints that are no current country: unknown (00), the Netherlands Antilles, Serbia and
# Montenegro, Yugoslavia (ISO 3166-3 withdrawn codes).
NO_COUNTRY_CODES = {'00', 'AN', 'CS', 'YU'}

# Identifiers (crd::comply's grammar kind[.cc]:value): kinds a holder's document or a registry issues per country
# carry the issuing country when the list names it; the others are unique worldwide. A kind never seen here and
# never listed as an attribute refuses the import.
COUNTRY_SCOPED = {'passport', 'national-id', 'tax-id', 'registration', 'driving-licence', 'birth-certificate',
                  'residence-permit', 'travel-document', 'voter-id', 'military-id', 'seafarer-id', 'licence',
                  'pilot-licence', 'bank-code'}

OFAC_IDENTIFIERS = {
    'passport': ['Passport', 'Diplomatic Passport', 'British National Overseas Passport', 'Stateless Person Passport'],
    'national-id': ['National ID No.', 'Personal ID Card', 'Cedula No.', 'D.N.I.', 'Turkish Identification Number',
                    'Kenyan ID No.', 'Tazkira National ID Card', 'CNP (Personal Numerical Code)', 'Numero de Identidad',
                    "Citizen's Card Number", 'Federal ID Card', 'Moroccan Personal ID No.', 'Bosnian Personal ID No.',
                    'Tarjeta de Identidad', 'UAE Identification', 'C.U.R.P.', 'National Foreign ID Number', 'N.I.E.',
                    'Stateless Person ID Card', 'Refugee ID Card', 'Identification Number', 'SSN', 'C.U.I.P.'],
    'tax-id': ['Tax ID No.', 'R.F.C.', 'RFC', 'NIT #', 'RUC #', 'V.A.T. Number', 'Italian Fiscal Code', 'Fiscal Code',
               'C.U.I.T.', 'C.U.I.', 'RIF #', 'N.I.F.', 'C.I.F.', 'US FEIN', 'Paraguayan tax identification number',
               'Numero Unico de Identificacao Tributaria (NUIT)', 'Romanian Tax Registration',
               'Global Intermediary Identification Number'],
    'registration': ['Registration Number', 'Registration ID', 'Business Registration Number', 'Company Number',
                     'Commercial Registry Number', 'Folio Mercantil No.', 'Matricula Mercantil No', 'Business Number',
                     'Enterprise Number', 'Chamber of Commerce Number', 'C.R. No.', 'Unified Social Credit Code (USCC)',
                     'United Social Credit Code Certificate (USCCC)', 'Economic Register Number (CBLS)',
                     'Government Gazette Number', 'Central Registration System Number', 'C.I.N.',
                     'Business Registration Document #', 'Istanbul Chamber of Comm. No.',
                     'Dubai Chamber of Commerce Membership No.', 'Registered Charity No.', 'UK Company Number',
                     'Public Registration Number', 'Certificate of Incorporation Number', 'Organization Code',
                     'Romanian C.R.', 'Entity Code', 'LE Number', 'Branch Unit Number',
                     'Registration Certificate Number (Dubai)',
                     'Russian State Individual Business Registration Number Pattern (OGRNIP)',
                     'MSB Registration Number',
                     'File Number', 'Trademark number', 'Codigo Alfanumerico Armonizado del Transportista (CAAT)',
                     'Standard Carrier Alpha Code (SCAC)'],
    'licence': ['License', 'Trade License No.', 'Afghan Money Service Provider License Number', 'Tourism License No.',
                'Permit Number', 'SRE Permit No.'],
    'residence-permit': ['Residency Number', 'Romanian Permanent Resident', 'Immigration No.',
                         'Public Security and Immigration No.', 'VisaNumberID'],
    'travel-document': ['Travel Document Number'], 'seafarer-id': ["Seafarer's Identification Document"],
    'pilot-licence': ['Pilot License Number'], 'driving-licence': ["Driver's License No."],
    'birth-certificate': ['Birth Certificate Number'],
    'voter-id': ['Electoral Registry No.', 'Credencial electoral', 'I.F.E.'],
    'military-id': ['Cartilla de Servicio Militar Nacional', 'Military Registration Number'],
    'lei': ['Legal Entity Number'], 'duns': ['D-U-N-S Number'], 'swift': ['SWIFT/BIC'], 'isin': ['ISIN'],
    'bank-code': ['BIK (RU)', 'RTN'],
    'imo': ['Vessel Registration Identification'], 'mmsi': ['MMSI'], 'call-sign': ['Other Vessel Call Sign'],
    'aircraft-serial': ["Aircraft Manufacturer's Serial Number (MSN)",
                        'Aircraft Construction Number (also called L/N or S/N or F/N)',
                        'Aircraft Serial Identification', 'Serial No.'],
    'aircraft-tail': ['Aircraft Tail Number', 'Previous Aircraft Tail Number'],
    'aircraft-mode-s': ['Aircraft Mode S Transponder Code'],
    'email': ['Email Address'], 'website': ['Website'],
}
OFAC_IDENTIFIER_KINDS = {name: kind for kind, names in OFAC_IDENTIFIERS.items() for name in names}
OFAC_CURRENCY = re.compile(r'^Digital Currency Address - ([A-Z0-9]{2,6})$')
# idList items that describe the party rather than identify it: sanctions information, dates, descriptions, a
# phone number (printed in fragments), a ticker or a location code; and the Chinese Commercial Code, which spells a
# name in telegraph code. Read, never compared. OFAC names a dated directive or programme note per programme, so
# those families are patterns.
OFAC_ATTRIBUTES = {
    'Secondary sanctions risk:', 'Gender', 'Organization Established Date', 'Additional Sanctions Information -',
    'Organization Type:', 'Target Type', 'Transactions Prohibited For Persons Owned or Controlled By U.S. Financial '
    'Institutions:', 'Executive Order 13662 Directive Determination -', 'CAATSA Section 235 Information:',
    'IFCA Determination -', 'PAIPA Section 2 Information:', 'PEESA Information:', 'HKAA Section 6 Information:',
    'Vessel Year of Build', 'Former Vessel Flag', 'Other Vessel Flag', 'Other Vessel Type',
    'Nationality of Registration', 'Registration Country', 'Aircraft Model', 'Aircraft Manufacture Date',
    'Aircraft Operator', 'Phone Number', 'Equity Ticker', 'MICEX Code', 'UN/LOCODE', 'Chinese Commercial Code',
    'Issuer Name',
}
OFAC_ATTRIBUTE_FAMILIES = re.compile(r'^(?:(?:Listing|Effective|Purchase/Sales For Divestment) Date \([^)]+\):?|'
                                     r'Executive Order \d+ (?:Directive )?[Ii]nformation:?(?: -)?)$')
EU_IDENTIFIERS = {'passport': 'passport', 'id': 'national-id', 'ssn': 'national-id', 'unssn': 'national-id',
                  'nationcert': 'national-id', 'fiscalcode': 'tax-id', 'taxid': 'tax-id', 'euvat': 'tax-id',
                  'regnumber': 'registration', 'tradelic': 'licence', 'imo': 'imo', 'swiftbic': 'swift',
                  'birthcert': 'birth-certificate', 'drivinglicence': 'driving-licence',
                  'residentperm': 'residence-permit', 'travelcardid': 'travel-document', 'electionid': 'voter-id'}
EU_ATTRIBUTES = {'other'}  # "Other identification number": no kind a subject could name
UN_IDENTIFIERS = {'passport': 'passport', 'national identification number': 'national-id'}
MONTHS = {name: number for number, names in enumerate(
    [('jan', 'january'), ('feb', 'february'), ('mar', 'march'), ('apr', 'april'), ('may',), ('jun', 'june'),
     ('jul', 'july'), ('aug', 'august'), ('sep', 'sept', 'september'), ('oct', 'october'), ('nov', 'november'),
     ('dec', 'december')], 1) for name in names}


# A birth the list gives as approximate (circa): a reader returns it for Party.birth.
APPROXIMATE = 'approximate'


class Party:
    def __init__(self, authority, reference, kind):
        if not AUTHORITY.match(str(reference)):
            raise SanctionsError(f'{authority}: the reference {reference!r} cannot be a party id')
        self.id = f'{authority}.{reference}'
        self.kind = kind
        self.names, self.programs, self.births, self.countries, self.identifiers = [], [], [], [], []
        self.listed = ''
        self.approximate = False

    def where(self):
        return f'party {self.id}'

    def name(self, *parts):
        text = ' '.join(' '.join(part.split()) for part in parts if part and part.strip())
        if text and any(char.isalnum() for char in text) and text not in self.names:
            self.names.append(text)

    def program(self, text):
        text = ' '.join((text or '').split())
        if text and text not in self.programs:
            self.programs.append(text)

    def birth(self, periods):
        """One period, a list of them, None (no period) or APPROXIMATE. An approximate birth leaves the party with no
        period at all: its exact births beside it would otherwise discount a subject born near the approximate one."""
        if periods == APPROXIMATE:
            self.approximate = True
            periods = None
        periods = [periods] if isinstance(periods, str) else periods or []
        if (periods or self.approximate) and self.kind != 'individual':
            raise SanctionsError(f'{self.where()}: a birth on a {self.kind}')
        for period in periods:
            if period not in self.births:
                self.births.append(period)

    def country(self, text, iso=False):
        code = country_code(text, iso, self.where())
        if code and code not in self.countries:
            self.countries.append(code)
        return code

    def identifier(self, kind, value, country=None):
        value = ' '.join((value or '').split())
        if kind == 'imo' or kind == 'mmsi':
            value = re.sub(r'^(?:IMO|MMSI)\s*', '', value, flags=re.IGNORECASE)
        if not re.search(r'[A-Za-z0-9]', value):
            return  # nothing comparable: the decoder's compared form keeps ASCII letters and digits
        qualified = f'{kind}.{country.lower()}' if kind in COUNTRY_SCOPED and country else kind
        text = f'{qualified}:{value}'
        if text not in self.identifiers:
            self.identifiers.append(text)

    def finished(self):
        if not self.names:
            raise SanctionsError(f'{self.where()}: no name')
        if not self.programs:
            raise SanctionsError(f'{self.where()}: no programme')
        if self.approximate:
            self.births = []
        self.countries.sort()
        for key, items, bound in (('names', self.names, MAX_NAMES), ('programs', self.programs, MAX_ITEMS),
                                  ('births', self.births, MAX_ITEMS), ('countries', self.countries, MAX_ITEMS),
                                  ('identifiers', self.identifiers, MAX_IDENTIFIERS)):
            if len(items) > bound:
                raise SanctionsError(f'{self.where()}: {len(items)} {key}, more than the {bound} a party may hold')
            for item in items:
                if len(item.encode('utf-8')) > MAX_TEXT:
                    raise SanctionsError(f'{self.where()}: a {key} value longer than {MAX_TEXT} bytes')
        return self


def country_code(text, iso, where):
    text = ' '.join((text or '').split())
    if not text:
        return None
    if iso:
        if text in NO_COUNTRY_CODES:
            return None
        if not re.fullmatch(r'[A-Z]{2}', text):
            raise SanctionsError(f'{where}: the country code {text!r}')
        return text
    if text.casefold() not in COUNTRY_NAMES:
        raise SanctionsError(f'{where}: the country {text!r} is not in {TOOL} COUNTRIES; add it')
    return COUNTRY_NAMES[text.casefold()]


def day_text(year, month=None, day=None, where=''):
    try:
        if day:
            return datetime.date(int(year), int(month), int(day)).isoformat()
        if month:
            datetime.date(int(year), int(month), 1)
            return f'{int(year):04d}-{int(month):02d}'
        if 1900 <= int(year) <= 9999:
            return f'{int(year):04d}'
    except ValueError:
        pass
    raise SanctionsError(f'{where}: the date {year}-{month}-{day}')


def bounds(period):
    """The first and last day a YYYY, YYYY-MM or YYYY-MM-DD covers."""
    parts = [int(part) for part in period.split('-')]
    first = datetime.date(parts[0], parts[1] if len(parts) > 1 else 1, parts[2] if len(parts) > 2 else 1)
    if len(parts) == 3:
        return first, first
    if len(parts) == 1 or parts[1] == 12:
        following = datetime.date(parts[0] + 1, 1, 1)
    else:
        following = datetime.date(parts[0], parts[1] + 1, 1)
    return first, following - datetime.timedelta(days=1)


def interval(first, last, where):
    """The period from `first` to `last` (YYYY[-MM[-DD]]/YYYY[-MM[-DD]]), or one of them when they are equal."""
    if first == last:
        return first
    if bounds(first)[0] > bounds(last)[1]:
        raise SanctionsError(f'{where}: the period {first} to {last} ends before it starts')
    return f'{first}/{last}'


def text_of(element, path, ns=''):
    found = element.find(ns + path.replace('/', '/' + ns))
    return ' '.join(found.text.split()) if found is not None and found.text else ''


def parse_xml(body, where):
    """The document's root, refusing a document type declaration (the official files carry none, so no entity is
    ever declared or expanded) before the tree is built."""
    if len(body) > FILE_LIMIT:
        raise SanctionsError(f'{where}: larger than {FILE_LIMIT} bytes')

    def refuse(*_):
        raise SanctionsError(f'{where}: a document type declaration; the official files carry none')

    checker = xml.parsers.expat.ParserCreate()
    checker.StartDoctypeDeclHandler = refuse
    try:
        checker.Parse(body, True)
        return ET.fromstring(body)
    except (ET.ParseError, xml.parsers.expat.ExpatError) as error:
        raise SanctionsError(f'{where}: not well-formed XML ({error})') from error


# OFAC ---------------------------------------------------------------------------------------------------------------

OFAC_NS = '{https://sanctionslistservice.ofac.treas.gov/api/PublicationPreview/exports/XML}'
OFAC_KINDS = {'Individual': 'individual', 'Entity': 'entity', 'Vessel': 'vessel', 'Aircraft': 'aircraft'}
OFAC_DAY = re.compile(r'^(?:(\d{1,2}) )?(?:([A-Z][a-z]{2}) )?(\d{4})$')


def ofac_period(text, where):
    match = OFAC_DAY.match(text)
    if not match or (match.group(1) and not match.group(2)):
        raise SanctionsError(f'{where}: the date of birth {text!r}')
    month = MONTHS.get((match.group(2) or '').lower()) if match.group(2) else None
    if match.group(2) and month is None:
        raise SanctionsError(f'{where}: the month in {text!r}')
    return day_text(match.group(3), month, match.group(1), where)


def ofac_birth(text, where):
    """OFAC's dateOfBirth: [DD ]Mon YYYY, YYYY, A to B, or circa A (APPROXIMATE)."""
    if text.startswith('circa '):
        ofac_period(text[len('circa '):], where)
        return APPROXIMATE
    first, _, last = text.partition(' to ')
    first = ofac_period(first, where)
    if not last:
        return first
    last = ofac_period(last, where)
    return interval(first, last, where)


def read_ofac(root, authority):
    ns = OFAC_NS
    if root.tag != ns + 'sdnList':
        raise SanctionsError(f'{authority}: the root is {root.tag}, not OFAC\'s sdnList')
    stamp = text_of(root, 'publshInformation/Publish_Date', ns)
    match = re.fullmatch(r'(\d{2})/(\d{2})/(\d{4})', stamp)
    if not match:
        raise SanctionsError(f'{authority}: the publish date {stamp!r}')
    published = day_text(match.group(3), match.group(1), match.group(2), authority)
    parties = []
    for entry in root.iter(ns + 'sdnEntry'):
        kind = OFAC_KINDS.get(text_of(entry, 'sdnType', ns))
        if kind is None:
            raise SanctionsError(f'{authority}: entry {text_of(entry, "uid", ns)} has the type '
                               f'{text_of(entry, "sdnType", ns)!r}')
        party = Party(authority, text_of(entry, 'uid', ns), kind)
        party.name(text_of(entry, 'firstName', ns), text_of(entry, 'lastName', ns))
        for aka in entry.iterfind(f'{ns}akaList/{ns}aka'):
            party.name(text_of(aka, 'firstName', ns), text_of(aka, 'lastName', ns))
        for program in entry.iterfind(f'{ns}programList/{ns}program'):
            party.program(program.text)
        for path in (f'{ns}addressList/{ns}address/{ns}country', f'{ns}nationalityList/{ns}nationality/{ns}country',
                     f'{ns}citizenshipList/{ns}citizenship/{ns}country', f'{ns}vesselInfo/{ns}vesselFlag'):
            for country in entry.iterfind(path):
                party.country(country.text)
        for item in entry.iterfind(f'{ns}dateOfBirthList/{ns}dateOfBirthItem'):
            party.birth(ofac_birth(text_of(item, 'dateOfBirth', ns), party.where()))
        for item in entry.iterfind(f'{ns}idList/{ns}id'):
            id_type = text_of(item, 'idType', ns)
            currency = OFAC_CURRENCY.match(id_type)
            if currency:
                party.identifier('crypto-' + currency.group(1).lower(), text_of(item, 'idNumber', ns))
            elif id_type in OFAC_IDENTIFIER_KINDS:
                country = party.country(text_of(item, 'idCountry', ns))
                party.identifier(OFAC_IDENTIFIER_KINDS[id_type], text_of(item, 'idNumber', ns), country)
            elif id_type not in OFAC_ATTRIBUTES and not OFAC_ATTRIBUTE_FAMILIES.match(id_type):
                raise SanctionsError(f'{party.where()}: the identifier type {id_type!r} is not in {TOOL}')
        call_sign = text_of(entry, 'vesselInfo/callSign', ns)
        if call_sign:
            party.identifier('call-sign', call_sign)
        parties.append(party.finished())
    count = text_of(root, 'publshInformation/Record_Count', ns)
    if count != str(len(parties)):
        raise SanctionsError(f'{authority}: {len(parties)} entries, but the file says {count}')
    return published, parties


# EU (the financial sanctions file, FSF 1.1) -------------------------------------------------------------------------

EU_NS = '{http://eu.europa.ec/fpi/fsd/export}'
OPEN_START, OPEN_END = '1900', '9999'  # the decoder's first and last years
EU_KINDS = {'person': 'individual', 'enterprise': 'entity'}


def eu_birth(element, where):
    """The periods of an FSF birthdate: a Gregorian date or year, a year range (an open end runs to the decoder's
    first or last year: wider, never narrower), or an Islamic-calendar date (the file's own Gregorian day when it
    gives one); circa marks an approximate date (APPROXIMATE)."""
    calendar = element.get('calendarType') or 'GREGORIAN'
    if calendar not in ('GREGORIAN', 'ISLAMIC'):
        raise SanctionsError(f'{where}: the calendar {calendar!r}')
    if element.get('circa') == 'true':
        return APPROXIMATE
    if element.get('birthdate'):
        match = re.fullmatch(r'(\d{4})-(\d{2})-(\d{2})', element.get('birthdate'))
        if not match:
            raise SanctionsError(f'{where}: the birth date {element.get("birthdate")!r}')
        return [day_text(*match.groups(), where=where)]
    year, month, day = (element.get(key) or '' for key in ('year', 'monthOfYear', 'dayOfMonth'))
    if calendar == 'ISLAMIC':
        if not year or (day and not month):
            raise SanctionsError(f'{where}: an Islamic birth date without a year')
        if day:
            return [hijri_period(int(day), int(month), int(year), where)]
        return hijri_year_periods(int(year), where)
    if year:
        return [day_text(year, month or None, day or None, where)]
    first, last = element.get('yearRangeFrom') or '', element.get('yearRangeTo') or ''
    if first or last:
        return [interval(day_text(first or OPEN_START, where=where), day_text(last or OPEN_END, where=where), where)]
    if day or month:
        raise SanctionsError(f'{where}: a birth date with a day or month and no year')
    return []  # an element naming only a place of birth


def instant_text(text, where):
    try:
        moment = datetime.datetime.fromisoformat(text.replace('Z', '+00:00'))
    except ValueError as error:
        raise SanctionsError(f'{where}: the generation time {text!r}') from error
    if moment.tzinfo is None:
        raise SanctionsError(f'{where}: the generation time {text!r} has no offset')
    return moment.astimezone(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')


def read_eu(root, authority):
    ns = EU_NS
    if root.tag != ns + 'export':
        raise SanctionsError(f'{authority}: the root is {root.tag}, not the FSF export')
    published = instant_text(root.get('generationDate') or '', authority)
    parties = []
    for entity in root.iterfind(ns + 'sanctionEntity'):
        subject = entity.find(ns + 'subjectType')
        kind = EU_KINDS.get(subject.get('code') if subject is not None else None)
        if kind is None:
            raise SanctionsError(f'{authority}: entity {entity.get("logicalId")} has no known subject type')
        party = Party(authority, entity.get('logicalId'), kind)
        for alias in entity.iterfind(ns + 'nameAlias'):
            whole = alias.get('wholeName') or ''
            if whole.strip():
                party.name(whole)
            else:
                party.name(alias.get('firstName'), alias.get('middleName'), alias.get('lastName'))
        listed = sorted(regulation.get('publicationDate') or '' for regulation in entity.iterfind(ns + 'regulation'))
        listed = [day for day in listed if day]
        if listed:
            if not re.fullmatch(r'\d{4}-\d{2}-\d{2}', listed[0]):
                raise SanctionsError(f'{party.where()}: the regulation day {listed[0]!r}')
            party.listed = listed[0]
        for regulation in entity.iterfind(ns + 'regulation'):
            party.program(regulation.get('programme'))
        for tag in ('citizenship', 'address'):
            for element in entity.iterfind(ns + tag):
                party.country(element.get('countryIso2Code'), iso=True)
        for element in entity.iterfind(ns + 'birthdate'):
            party.birth(eu_birth(element, party.where()))
        for element in entity.iterfind(ns + 'identification'):
            code = element.get('identificationTypeCode') or ''
            if code in EU_IDENTIFIERS:
                country = party.country(element.get('countryIso2Code'), iso=True)
                party.identifier(EU_IDENTIFIERS[code], element.get('number'), country)
            elif code not in EU_ATTRIBUTES:
                raise SanctionsError(f'{party.where()}: the identification type {code!r} is not in {TOOL}')
        parties.append(party.finished())
    return published, parties


# UN (the Security Council consolidated list) ------------------------------------------------------------------------

UN_NOTE = re.compile(r'^(?:([A-Za-z]+)\.? )?(\d{4})$')
UN_HIJRI = re.compile(r'^(\d{1,2})/(\d{1,2})/(\d{4}) \(Hijri Calendar\)$')


HIJRI_EPOCH = datetime.date(622, 7, 19)  # 1 Muharram 1 AH in the tabular calendar (proleptic Gregorian)
HIJRI_SLACK = datetime.timedelta(days=2)  # the observed and Umm al-Qura months start up to two days from it


def hijri_day(year, month, day):
    """The tabular (arithmetic) Islamic calendar's Gregorian day."""
    return HIJRI_EPOCH + datetime.timedelta(days=(year - 1) * 354 + (3 + 11 * year) // 30 + 29 * (month - 1) +
                                            month // 2 + day - 1)


def hijri_period(day, month, year, where):
    """A Hijri date as the Gregorian days it can be: the tabular day, two days either side."""
    if not (1 <= month <= 12 and 1 <= day <= 30 and 1 <= year <= 9000):
        raise SanctionsError(f'{where}: the Hijri date {day}/{month}/{year}')
    middle = hijri_day(year, month, day)
    return interval((middle - HIJRI_SLACK).isoformat(), (middle + HIJRI_SLACK).isoformat(), where)


def hijri_year_periods(year, where):
    """An Islamic-calendar year a list does not say is lunar or solar (Iranian births are printed in the Solar
    Hijri year): the lunar year's days and the solar year's (Nowruz to Nowruz, 20 or 21 March), each two days
    wider."""
    if not 1 <= year <= 9000:
        raise SanctionsError(f'{where}: the Hijri year {year}')
    lunar = interval((hijri_day(year, 1, 1) - HIJRI_SLACK).isoformat(),
                     (hijri_day(year + 1, 1, 1) - datetime.timedelta(days=1) + HIJRI_SLACK).isoformat(), where)
    solar = interval(datetime.date(year + 621, 3, 18).isoformat(), datetime.date(year + 622, 3, 22).isoformat(),
                     where)
    return [lunar, solar]


def un_birth(element, where):
    kind = text_of(element, 'TYPE_OF_DATE')
    date, year = text_of(element, 'DATE'), text_of(element, 'YEAR')
    first, last, note = text_of(element, 'FROM_YEAR'), text_of(element, 'TO_YEAR'), text_of(element, 'NOTE')
    if kind == 'APPROXIMATELY':
        return APPROXIMATE
    if kind == 'BETWEEN':
        if not first or not last:
            raise SanctionsError(f'{where}: a BETWEEN birth without both years')
        return interval(day_text(first, where=where), day_text(last, where=where), where)
    if kind == '' and not (date or year or first or last or note):
        return None
    if kind != 'EXACT':
        raise SanctionsError(f'{where}: the birth type {kind!r}')
    if date:
        match = re.fullmatch(r'(\d{4})-(\d{2})-(\d{2})', date)
        if not match:
            raise SanctionsError(f'{where}: the birth date {date!r}')
        return day_text(*match.groups(), where=where)
    if year:
        return day_text(year, where=where)
    hijri = UN_HIJRI.match(note)
    if hijri:
        return hijri_period(*(int(part) for part in hijri.groups()), where=where)
    match = UN_NOTE.match(note)
    month = MONTHS.get(match.group(1).lower()) if match and match.group(1) else None
    if match and (not match.group(1) or month is not None):
        return day_text(match.group(2), month, None, where)
    if re.search(r'\d{4}', note):
        raise SanctionsError(f'{where}: the birth note {note!r}')
    return None  # a note without a year ("Sep.", "from false passport") names no period


def read_un(root, authority):
    if root.tag != 'CONSOLIDATED_LIST':
        raise SanctionsError(f'{authority}: the root is {root.tag}, not the UN CONSOLIDATED_LIST')
    published = instant_text(root.get('dateGenerated') or '', authority)
    parties = []
    for group, tag, kind in (('INDIVIDUALS', 'INDIVIDUAL', 'individual'), ('ENTITIES', 'ENTITY', 'entity')):
        for element in root.iterfind(f'{group}/{tag}'):
            party = Party(authority, text_of(element, 'REFERENCE_NUMBER').lower(), kind)
            party.name(*(text_of(element, part) for part in ('FIRST_NAME', 'SECOND_NAME', 'THIRD_NAME',
                                                              'FOURTH_NAME')))
            for alias in element.iterfind(f'{tag}_ALIAS'):
                party.name(text_of(alias, 'ALIAS_NAME'))
            party.name(text_of(element, 'NAME_ORIGINAL_SCRIPT'))
            party.program(text_of(element, 'UN_LIST_TYPE'))
            listed = text_of(element, 'LISTED_ON')
            if not re.match(r'\d{4}-\d{2}-\d{2}(?:$|T|[+-]\d{2}:\d{2}$)', listed):
                raise SanctionsError(f'{party.where()}: the listing day {listed!r}')
            party.listed = listed[:10]
            for value in element.iterfind('NATIONALITY/VALUE'):
                party.country(value.text)
            for address in element.iterfind(f'{tag}_ADDRESS'):
                party.country(text_of(address, 'COUNTRY'))
            for birth in element.iterfind(f'{tag}_DATE_OF_BIRTH'):
                party.birth(un_birth(birth, party.where()))
            for document in element.iterfind(f'{tag}_DOCUMENT'):
                kind_text = text_of(document, 'TYPE_OF_DOCUMENT')
                if not kind_text and not text_of(document, 'NUMBER'):
                    continue
                if kind_text.lower() not in UN_IDENTIFIERS:
                    raise SanctionsError(f'{party.where()}: the document type {kind_text!r} is not in {TOOL}')
                country = party.country(text_of(document, 'ISSUING_COUNTRY') or text_of(document, 'COUNTRY_OF_ISSUE'))
                party.identifier(UN_IDENTIFIERS[kind_text.lower()], text_of(document, 'NUMBER'), country)
            parties.append(party.finished())
    return published, parties


# UK (the UK Sanctions List) -----------------------------------------------------------------------------------------

UK_KINDS = {'Individual': 'individual', 'Entity': 'entity', 'Ship': 'vessel'}
UK_DATE = re.compile(r'^(\d{2}|dd)/(\d{2}|mm)/(\d{4}|\d{2}yy)$')  # dd, mm and 00 are unknown parts
UK_NUMBER = re.compile(r'[A-Za-z0-9]*\d[A-Za-z0-9-]*\d[A-Za-z0-9]*')


def uk_birth(text, where):
    if re.fullmatch(r'\d{4}', text):
        return day_text(text, where=where)
    match = UK_DATE.match(text)
    day, month, year = match.groups() if match else ('', '', '')
    day = None if day in ('dd', '00') else day
    month = None if month in ('mm', '00') else month
    if not match or (day and not month):
        raise SanctionsError(f'{where}: the date of birth {text!r}')
    if year.endswith('yy'):
        return f'{year[:2]}00/{year[:2]}99'  # the century: a day and month without their year narrow nothing
    return day_text(year, month, day, where)


def read_uk(root, authority):
    if root.tag != 'Designations':
        raise SanctionsError(f'{authority}: the root is {root.tag}, not the UK Designations')
    match = re.fullmatch(r'(\d{2})/(\d{2})/(\d{4})', text_of(root, 'DateGenerated'))
    if not match:
        raise SanctionsError(f'{authority}: the generation day {text_of(root, "DateGenerated")!r}')
    published = day_text(match.group(3), match.group(2), match.group(1), authority)
    parties = []
    for designation in root.iterfind('Designation'):
        kind = UK_KINDS.get(text_of(designation, 'IndividualEntityShip'))
        if kind is None:
            raise SanctionsError(f'{authority}: {text_of(designation, "UniqueID")} is a '
                               f'{text_of(designation, "IndividualEntityShip")!r}')
        party = Party(authority, text_of(designation, 'UniqueID').lower(), kind)
        for name in designation.iterfind('Names/Name'):
            party.name(*(text_of(name, f'Name{part}') for part in range(1, 7)))
        for name in designation.iterfind('NonLatinNames/NonLatinName'):
            party.name(text_of(name, 'NameNonLatinScript'))
        party.program(text_of(designation, 'RegimeName'))
        listed = re.fullmatch(r'(\d{2})/(\d{2})/(\d{4})', text_of(designation, 'DateDesignated'))
        if not listed:
            raise SanctionsError(f'{party.where()}: the designation day {text_of(designation, "DateDesignated")!r}')
        party.listed = day_text(listed.group(3), listed.group(2), listed.group(1), party.where())
        for path in ('IndividualDetails/Individual/Nationalities/Nationality', 'Addresses/Address/AddressCountry',
                     'ShipDetails/Ship/CurrentBelievedFlagOfShips/CurrentBelievedFlagOfShip'):
            for country in designation.iterfind(path):
                party.country(country.text)
        for birth in designation.iterfind('IndividualDetails/Individual/DOBs/DOB'):
            party.birth(uk_birth(' '.join((birth.text or '').split()), party.where()))
        for path, kind_name in (('IndividualDetails/Individual/PassportDetails/Passport/PassportNumber', 'passport'),
                                ('IndividualDetails/Individual/NationalIdentifierDetails/NationalIdentifier/'
                                 'NationalIdentifierNumber', 'national-id'),
                                ('ShipDetails/Ship/IMONumbers/IMONumber', 'imo'),
                                ('EmailAddresses/EmailAddress', 'email'), ('Websites/Website', 'website')):
            for value in designation.iterfind(path):
                party.identifier(kind_name, value.text)
        # A free-text field such as "(INN): 190950894 (Belarus), 7704734000/770301001 (Russia)": each number in it.
        for value in designation.iterfind('EntityDetails/Entity/BusinessRegistrationNumbers/'
                                          'BusinessRegistrationNumber'):
            for number in UK_NUMBER.findall(value.text or ''):
                party.identifier('registration', number)
        parties.append(party.finished())
    return published, parties


# Turkey (MASAK's asset-freeze table) --------------------------------------------------------------------------------
#
# MASAK publishes Turkey's asset freezes (Law No. 6415 Articles 5, 6 and 7, Law No. 7262) as one numbered table in its
# web application (masak.hmb.gov.tr), served by the application's search and by no file: each list of the register
# posts its sanction type to the search (`post`) and keeps the answer. The answer carries no publication stamp and an
# elapsedTime that changes on every request, so the fetch keeps it in canonical form (that field dropped, keys sorted)
# and the instant it was read is its publication; the fetch also reads the application's sanction types first and
# refuses a type no list of the register posts, so a new regime is read, never missed.
#
# The table is typed by hand and names no kind: a party is an individual when its row gives a birth year or a
# parent's name (a natural person's), a vessel when it gives an IMO number, else of unknown kind (crd::comply's
# Unknown, which no subject's kind contradicts). Births are free text in which day.month.year and month/day/year sit
# side by side, so a numeric date read both ways records both days; a birth read only in part, or approximate,
# records no period (it never discounts a hit). The identifier column holds a TCKN, VKN, GKN or passport number, or
# for the UN's parties the UN's documents written out in Turkish: a number after a word naming its kind ("pasaport",
# "ulusal kimlik", "VKN:", "IMO number:") is of that kind; any other has 11 digits with the TCKN check digits (a
# national-id.tr; a GKN shares the algorithm), 10 with the VKN check digit (a tax-id.tr; a TCKN and a VKN typed
# together are both) or is a passport; telephone and fax numbers, dates and notes are never compared. Countries are
# Turkish names, each known or the import is refused.

MASAK_TYPES = 'https://masak.hmb.gov.tr/portal/v2/masak/mvd/yaptirim'
MASAK_COLUMNS = ['sira_no', 'ad_soyad_unvan', 'tckn_vkn', 'uyrugu', 'yaptirim_turu', 'anne_adı', 'baba_adı',
                 'dogum_tarihi', 'dogum_yeri', 'orgutu', 'karar_sayisi', 'diger_adlari', 'diger_uyruk']
MASAK_ENVELOPE = {'statusCode', 'statusDescription', 'message', 'columnNames', 'numRows', 'data', 'result', 'errors'}
MASAK_TIMING = 'elapsedTime'  # the server's milliseconds, different on every answer
TR_FOLD = str.maketrans('ÇĞİIÖŞÜÂÎÛçğıöşüâîû', 'cgiiosuaiucgiosuaiu')
TR_COUNTRIES = {
    'AF': ['Afganistan'], 'AL': ['Arnavutluk'], 'AT': ['Avusturya'], 'AU': ['Avustralya'],
    'AZ': ['Azerbaycan', 'Azerbaidjan Republic'], 'BA': ['Bosna Hersek'], 'BD': ['Bangladeş'], 'BE': ['Belçika'],
    'CH': ['İsviçre'], 'CN': ['Çin'], 'DE': ['Almanya'], 'DK': ['Danimarka'], 'DZ': ['Cezayir'], 'EG': ['Mısır'],
    'ET': ['Etiyopya'], 'FR': ['Fransa', 'Fransız'],
    'GB': ['Birleşik Krallık', 'Büyük Britanya', 'İngiltere', 'Kuzey İrlanda', 'Kuzey İrlanda Birleşik Krallığı'],
    'GE': ['Gürcistan'], 'GR': ['Yunanistan'], 'ID': ['Endonezya'], 'IN': ['Hindistan'], 'IQ': ['Irak'],
    'IR': ['İran'], 'IT': ['İtalya'], 'JO': ['Ürdün'], 'JP': ['Japonya'], 'KE': ['Kenya'], 'KM': ['Komor Adaları'],
    'KP': ['Kore Demokratik Halk Cumhuriyeti'], 'KW': ['Kuveyt'], 'LB': ['Lübnan'], 'LY': ['Libya'], 'MA': ['Fas'],
    'MH': ['Marshall Adaları'], 'ML': ['Mali'], 'MR': ['Moritanya'], 'MY': ['Malezya'], 'NE': ['Nijer'],
    'NG': ['Nijerya'], 'NL': ['Hollanda'], 'NO': ['Norveç'], 'NZ': ['Yeni Zelanda'], 'PA': ['Panama'],
    'PH': ['Filipinler'], 'PK': ['Pakistan'], 'PS': ['Filistin'], 'QA': ['Katar'], 'RO': ['Romanya'],
    # The Russian Federation (RF) and the federal subjects the table prints in its place.
    'RU': ['Rusya', 'RF', 'Bashkortostan', 'Chechen Region', 'Dagestan Region', 'Kabardono-Balkaria Region',
           'Tatarstan Region', 'Voronezh Region'],
    'SA': ['Suudi Arabistan'], 'SE': ['İsveç'], 'SG': ['Singapur'], 'SN': ['Senegal'], 'SO': ['Somali'],
    'SY': ['Suriye'], 'TJ': ['Tacikistan'], 'TN': ['Tunus'],
    'TR': ['Türkiye Cumhuriyeti', 'Türkiye Cumhuriyeti Devleti'],
    'TT': ['Trinidad ve Tobago', 'Trnidad ve Tobago'], 'TZ': ['Tanzanya'], 'UG': ['Uganda'],
    'US': ['ABD', 'United States of America'], 'UZ': ['Özbekistan'], 'YE': ['Yemen'],
    # The Soviet Union, and Oran (an Algerian city the table prints beside Algeria): no current country.
    None: ['USSR', 'Oran'],
}
# Month names, folded: Turkish, and the English the UN's entries keep (MONTHS).
TR_MONTHS = dict(MONTHS, **{name: number for number, name in enumerate(
    ('ocak', 'subat', 'mart', 'nisan', 'mayis', 'haziran', 'temmuz', 'agustos', 'eylul', 'ekim', 'kasim', 'aralik'),
    1)})
# Dates in a folded birth field: a numeric day and month in either order, a day and month name, a span of years, a
# year; each standing alone (no digit before or after it).
MASAK_MONTH = '|'.join(sorted(TR_MONTHS, key=len, reverse=True))
MASAK_DATE = re.compile(r'(?<!\d)(?:(?P<a>\d{1,2})\s*[./]+\s*(?P<b>\d{1,2})\s*[./]+\s*(?P<y>\d{4})'
                        rf'|(?P<day>\d{{1,2}})\s+(?P<month>{MASAK_MONTH})\.?\s*(?P<named>\d{{4}})'
                        r'|(?P<first>\d{4})\s*-\s*(?P<last>\d{4})|(?P<year>\d{4}))(?!\d)')
MASAK_APPROXIMATE = {'tahmini', 'tahminen', 'yaklasik', 'muhtemelen', 'civari', 'olarak'}
MASAK_FILLER = {'arasi', 'veya', 've'}  # "1969-1971 arası" (between), "1961 veya 1962" (or)
MASAK_ENUMERATION = re.compile(r'(?<![a-z])[a-z]\)')  # a) b) c), glued to a number before them or not
# The words of the identifier column (folded) that say what the numbers after them are, longest first; None: numbers
# never compared (a telephone, a fax, a prison register, a ration book). Numbers before any of them are typed by
# their check digits.
MASAK_KINDS = {
    'tckn': 'national-id.tr', 'tck': 'national-id.tr', 'gkn': 'national-id.tr', 'ykn': 'national-id.tr',
    'vkn': 'tax-id.tr', 'yabanci ulke vkn': 'tax-id',
    'imo number': 'imo', 'swift/bic': 'swift', 'email': 'email', 'e-mail': 'email',
    'pasaport': 'passport', 'pasaportu': 'passport', 'pasaprot': 'passport', 'passport': 'passport', 'pn': 'passport',
    'seyahat belge': 'passport', 'seyahat belgesi': 'passport', 'reiseausweis': 'passport',
    'kimlik': 'national-id', 'tazkira': 'national-id', 'ssn': 'national-id', 'ehliyet': 'driving-licence',
    'mali kodu': 'tax-id', 'telephone': None, 'fax': None, 'kayit numarasi': None, 'karne': None,
}
MASAK_KIND = re.compile(r'(?<![a-z])(' + '|'.join(sorted(map(re.escape, MASAK_KINDS), key=len, reverse=True)) +
                        r')(?![a-z0-9])')  # glued to a number before it or not ("TCKN:31849452040VKN:5980156465")
# Words that are no passport series, though one to three letters before a number ("pasaport no 123456"); nor is a
# country ("Çin 653225197110100533").
MASAK_NOT_SERIES = {'no', 'nr', 've', 'de', 'da', 'ile', 'the', 'and'}
# Notes the table prints in parentheses beside a name: the earlier listed name, the name's initials, "third".
MASAK_NAME_NOTES = {'daha once listelenen isim', 'isminin ilk harfleri', 'ucuncu'}
MASAK_NAME_LABEL = re.compile(r'(?:ori?jinal yazimi?|eski adi)\s*:')  # in folded text
MASAK_LEGAL_TAILS = {'ltd', 'co', 'inc', 'llc', 'sti', 'ltd sti', 'as', 'co ltd'}
MASAK_NO_ORGANISATION = {'', 'bilinmiyor', 'orgutu yoktur'}  # unknown, none


def tr_fold(text):
    """Lower case without Turkish or circumflexed letters, spaces collapsed: the form names and notes are compared
    in."""
    return ' '.join(text.translate(TR_FOLD).lower().split())


TR_COUNTRY_NAMES = {tr_fold(name): code for code, names in TR_COUNTRIES.items() for name in names}


def tckn_valid(digits):
    """The check digits of a Turkish identity number (TCKN) or a foreigner's (GKN)."""
    if len(digits) != 11 or not digits.isdigit() or digits[0] == '0':
        return False
    d = [int(char) for char in digits]
    return (sum(d[0:9:2]) * 7 - sum(d[1:8:2])) % 10 == d[9] and sum(d[:10]) % 10 == d[10]


def vkn_valid(digits):
    """The check digit of a Turkish tax number (VKN)."""
    if len(digits) != 10 or not digits.isdigit():
        return False
    total = 0
    for i, digit in enumerate(int(char) for char in digits[:9]):
        shifted = (digit + 9 - i) % 10
        value = shifted * 2 ** (9 - i) % 9
        total += 9 if shifted and value == 0 else value
    return (10 - total % 10) % 10 == int(digits[9])


def masak_answer(body, where, columns, timed):
    """The rows of an answer of MASAK's application, its envelope exactly as served (with elapsedTime when `timed`,
    the raw answer, else canonical) and every row holding exactly `columns` (the number a positive integer, every
    other column text)."""
    if len(body) > FILE_LIMIT:
        raise SanctionsError(f'{where}: larger than {FILE_LIMIT} bytes')

    def refuse_constant(name):
        raise SanctionsError(f'{where}: the JSON constant {name}')

    try:
        document = json.loads(body.decode('utf-8'), parse_constant=refuse_constant)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise SanctionsError(f'{where}: not a JSON answer in UTF-8 ({error})') from error
    envelope = MASAK_ENVELOPE | ({MASAK_TIMING} if timed else set())
    if not isinstance(document, dict) or set(document) != envelope:
        raise SanctionsError(f'{where}: the answer holds {", ".join(sorted(envelope))}')
    if (document['statusCode'], document['statusDescription'], document['message'], document['result'],
            document['errors']) != (200, 'OK', None, None, []):
        raise SanctionsError(f'{where}: the answer is no success ({document["statusCode"]!r}, '
                             f'{document["statusDescription"]!r}, {document["errors"]!r})')
    if document['columnNames'] != columns:
        raise SanctionsError(f'{where}: the columns are {document["columnNames"]!r}, not {columns!r}')
    rows = document['data']
    if not isinstance(rows, list) or type(document['numRows']) is not int or document['numRows'] != len(rows):
        raise SanctionsError(f'{where}: numRows is not the number of rows')
    for row in rows:
        if not isinstance(row, dict) or set(row) != set(columns):
            raise SanctionsError(f'{where}: a row without exactly the columns {", ".join(columns)}')
        for column in columns:
            number = column == 'sira_no'
            if (type(row[column]) is not int or row[column] < 1) if number else not isinstance(row[column], str):
                raise SanctionsError(f'{where}: row {row.get("sira_no")!r} has the {column} {row[column]!r}')
    return rows


def masak_canonical(body, where):
    """The answer as kept: elapsedTime dropped, keys sorted, UTF-8, so a later answer with the same rows is the same
    bytes."""
    document = json.loads(body.decode('utf-8'))
    masak_answer(body, where, MASAK_COLUMNS, timed=True)
    del document[MASAK_TIMING]
    return (json.dumps(document, ensure_ascii=False, sort_keys=True, indent=1) + '\n').encode('utf-8')


def masak_census(download, configs):
    """Refuses when the sanction types MASAK's application offers are not the types the register's lists post."""
    offered = {row['yaptirim_turu'] for row in masak_answer(download(MASAK_TYPES), MASAK_TYPES, ['yaptirim_turu'],
                                                            timed=True)}
    posted = {(config.get('post') or {}).get('yaptirim') for config in configs}
    if offered != posted:
        raise SanctionsError(f'{MASAK_TYPES} offers the sanction types {sorted(offered)!r}, the register posts '
                             f'{sorted(posted, key=str)!r}: read the new type and add or retire its list')


def try_day(year, month=None, day=None):
    try:
        return day_text(year, month, day)
    except SanctionsError:
        return None


def masak_births(text):
    """(periods, has a year) of a birth field. The periods are every day a numeric date can be read as, each named
    date, span of years and year, or None when the field holds anything else (a name, a place, a note) or is
    approximate."""
    folded = tr_fold(text.replace('"', ' '))
    periods, rest, at = [], [], 0
    for match in MASAK_DATE.finditer(folded):
        rest.append(folded[at:match.start()])
        at = match.end()
        if match.group('y'):
            first, second, year = (int(match.group(key)) for key in ('a', 'b', 'y'))
            found = [day for day in (try_day(year, second, first), try_day(year, first, second)) if day]
        elif match.group('named'):
            found = [try_day(match.group('named'), TR_MONTHS[match.group('month')], match.group('day'))]
        elif match.group('first'):
            first, last = sorted((match.group('first'), match.group('last')))
            found = [interval(first, last, '') if try_day(first) and try_day(last) else None]
        else:
            found = [try_day(match.group('year'))]
        if not found or None in found:
            return None, True
        periods += [period for period in found if period not in periods]
    rest.append(folded[at:])
    words = re.sub(r"[\s,;.:/()'\-]+", ' ', MASAK_ENUMERATION.sub(' ', ' '.join(rest))).split()
    has_year = bool(periods)
    if any(word in MASAK_APPROXIMATE for word in words) or any(word not in MASAK_FILLER for word in words):
        return None, has_year or bool(re.search(r'(?<!\d)\d{4}(?!\d)', folded))
    return periods, has_year


def masak_identifiers(party, text):
    """Each identifier of the identifier column, its days dropped: the numbers after a word naming a kind of that kind
    (the kinds above), a series of one to three letters joined to the number after it ("D 000857"), two numbers
    joined by a dash both read ("36113043528-9611118807"), numbers of fewer than 6 characters dropped as notes."""
    folded = re.sub(r'[()]', ' ', MASAK_ENUMERATION.sub(' ', tr_fold(text)))
    days = MASAK_DATE.sub(lambda date: ' ' if date.group('y') or date.group('named') else date.group(0), folded)
    parts = MASAK_KIND.split(days)
    for word, segment in zip([None, *parts[1::2]], parts[0::2]):
        kind = MASAK_KINDS[word] if word else ''
        if kind == 'email':
            for address in re.findall(r'[\w.+-]+@[\w-]+(?:\.[\w-]+)+', segment):
                party.identifier('email', address)
            continue
        if kind is None:
            continue
        tokens = re.findall(r'[a-z0-9]+(?:-[a-z0-9]+)*', segment)
        for at, token in enumerate(tokens):
            series = token not in MASAK_NOT_SERIES and token not in TR_COUNTRY_NAMES
            if series and re.fullmatch(r'[a-z]{1,3}(?:-[a-z])?', token) and at + 1 < len(tokens) and \
                    tokens[at + 1][0].isdigit():
                tokens[at + 1] = token + tokens[at + 1]
        for token in tokens:
            parts = token.split('-')
            for part in parts if len(parts) > 1 and all(len(part) >= 6 for part in parts) else [token]:
                compact = part.replace('-', '')
                if kind == 'swift' and re.fullmatch(r'[a-z0-9]{8}(?:[a-z0-9]{3})?', compact):
                    party.identifier('swift', part)
                elif len(compact) >= 6 and re.search(r'\d', compact):
                    masak_number(party, kind, part)


def masak_number(party, kind, token):
    """A number the column's word names the kind of (`kind`), or one typed by its check digits (`kind` empty): a TCKN
    or GKN, a VKN, both typed together (21 digits), else a passport, and also a VKN when 9 digits with a leading zero
    pass its check (the table drops a VKN's leading zero in 11 of the domestic list's 14 such numbers, 2026-11-01). A
    Turkish number's check digits outrank its word ("YKN: 5240949089" is a VKN)."""
    if kind.endswith('.tr') and (tckn_valid(token) or vkn_valid(token)):
        kind = ''
    if kind:
        kind, _, country = kind.partition('.')
        if kind == 'national-id' and tckn_valid(token):
            country = 'tr'
        party.identifier(kind, token, country.upper() or None)
        return
    glued = [(token[:11], token[11:]), (token[:10], token[10:])] if len(token) == 21 else []
    pair = next((pair for pair in glued if {tckn_valid(part) or vkn_valid(part) for part in pair} == {True}), None)
    for part in pair or (token,):
        if tckn_valid(part):
            party.identifier('national-id', part, 'TR')
        elif vkn_valid(part):
            party.identifier('tax-id', part, 'TR')
        else:
            party.identifier('passport', part)
            if len(part) == 9 and vkn_valid(f'0{part}'):
                party.identifier('tax-id', f'0{part}', 'TR')  # a VKN whose leading zero the table lost, or not


def masak_names(text):
    """The names of a name or alias field: split at commas, semicolons, line ends, spaced dashes, a) b) c), 1- 2- 3-
    and the labels "ORJİNAL YAZIM:" (original script) and "ESKİ ADI:" (former name) outside parentheses; each with
    its parentheses dropped, and each parenthesis an alias of its own unless it is a note or a country."""
    text = text.replace('<', ' ').replace('>', ' ')
    for label in reversed(list(MASAK_NAME_LABEL.finditer(text.translate(TR_FOLD).lower()))):
        text = f'{text[:label.start()]},{text[label.end():]}'  # translate and lower() keep each index here
    # A list numbered 1-, 2-, 3-, a number glued to the name before it or not ("2- MOHAMAD ALUOALİ3-WALİD AYSSA").
    number, cut = 1, []
    for match in re.finditer(r'(?<!\d)(\d{1,2})-\s*', text):
        if int(match.group(1)) == number:
            cut.append(match.span())
            number += 1
    for start, end in reversed(cut):
        text = f'{text[:start]},{text[end:]}'
    pieces, current, notes, depth, marker = [], [], [], 0, 'a'
    for char in text:
        if char == '(':
            depth += 1
            if depth == 1:
                notes.append([])
                continue
        elif char == ')' and depth:
            depth -= 1
            if not depth:
                continue
        if depth:
            notes[-1].append(char)
        elif char in '),;\n':
            piece = ''.join(current)
            # A ')' outside parentheses closes an enumeration's letter, which is no part of the name: the letter
            # next in order, even glued to a name ("a) Abu Thorjqb) Rusdjan"), or any letter standing alone.
            if char == ')' and piece[-1:].lower() == marker:
                piece, marker = piece[:-1], chr(ord(marker) + 1)
            elif char == ')':
                piece = re.sub(r'(?<![^\W\d_])[^\W\d_]$', '', piece)
            pieces.append(piece)
            current = []
        else:
            current.append(char)
    pieces.append(''.join(current))
    names = []
    for name in (name.strip(' .:') for piece in pieces for name in re.split(r'\s[-–]\s', piece)):
        if names and re.sub(r'[^a-z ]', '', tr_fold(name)).strip() in MASAK_LEGAL_TAILS:
            names[-1] = f'{names[-1]}, {name}'  # "CO., LTD": a legal form after a comma ends the name before it
        else:
            names.append(name)
    for note in (' '.join(''.join(chars).split()) for chars in notes):
        if tr_fold(note) not in MASAK_NAME_NOTES:
            names += masak_names(note)
    # A lone letter is no name, nor is a country (a place beside a name, "(PAKISTAN)", or in an address).
    return [name for name in names if len(re.sub(r'\W', '', name)) > 1 and tr_fold(name) not in TR_COUNTRY_NAMES and
            name.casefold() not in COUNTRY_NAMES]


def masak_country(party, text):
    for name in re.split(r'[,\n]', text):
        key = tr_fold(name)
        if not key:
            continue
        if key not in TR_COUNTRY_NAMES:
            raise SanctionsError(f'{party.where()}: the country {name.strip()!r} is not in {TOOL} TR_COUNTRIES; add it')
        code = TR_COUNTRY_NAMES[key]
        if code and code not in party.countries:
            party.countries.append(code)


def read_masak(body, authority, record, config):
    sanction = (config.get('post') or {}).get('yaptirim')
    if not isinstance(sanction, str) or not sanction:
        raise SanctionsError(f'{authority}: post names the sanction type (yaptirim) the list is')
    rows = masak_answer(body, authority, MASAK_COLUMNS, timed=False)
    published = instant_text((record or {}).get('instant') or '', authority)
    parties = []
    for row in rows:
        if row['yaptirim_turu'] != sanction:
            raise SanctionsError(f'{authority}: row {row["sira_no"]} is a {row["yaptirim_turu"]!r}, not {sanction!r}')
        births, has_year = masak_births(row['dogum_tarihi'])
        parents = (row['anne_adı'] + row['baba_adı']).strip()
        kind = 'individual' if has_year or parents else \
            'vessel' if re.search(r'imo number\s*:', row['tckn_vkn'], re.IGNORECASE) else 'unknown'
        party = Party(authority, row['sira_no'], kind)
        for name in masak_names(row['ad_soyad_unvan']) + masak_names(row['diger_adlari']):
            party.name(name)
        party.program(row['yaptirim_turu'])
        if tr_fold(row['orgutu']) not in MASAK_NO_ORGANISATION:
            party.program(row['orgutu'])
        # The Official Gazette's day and issue ("2024-06-14 / 32576 (Mükerrer)"), or its issue alone: no listing day.
        decision = ' '.join(row['karar_sayisi'].split())
        listed = re.match(r'(?:(\d{4})-(\d{2})-(\d{2}) )?/ ?\S', decision)
        if decision and not listed:
            raise SanctionsError(f'{party.where()}: the decision {decision!r} is no Official Gazette day and number')
        party.listed = day_text(*listed.groups(), where=party.where()) if listed and listed.group(1) else ''
        party.birth(births)
        masak_country(party, row['uyrugu'])
        masak_country(party, row['diger_uyruk'])
        masak_identifiers(party, row['tckn_vkn'])
        parties.append(party.finished())
    return published, parties


class Format(NamedTuple):
    suffix: str  # the fetched file's: <authority>.<suffix>
    read: Callable  # (body, authority, fetch record, list config) -> (publication, parties)
    canonical: Optional[Callable] = None  # (answer, authority) -> the bytes kept, for an answer that varies
    census: Optional[Callable] = None  # (download, the register's lists of the format): refuses an unread type
    posts: bool = False  # the URL is an application's search, posted the list's `post`


def xml_format(reader):
    return Format('xml', lambda body, authority, record, config: reader(parse_xml(body, authority), authority))


READERS = {'ofac-xml': xml_format(read_ofac), 'eu-fsf-xml': xml_format(read_eu), 'un-sc-xml': xml_format(read_un),
           'gb-uksl-xml': xml_format(read_uk),
           'tr-masak-json': Format('json', read_masak, masak_canonical, masak_census, posts=True)}


# Generations --------------------------------------------------------------------------------------------------------

def generation_of(published, previous, sha256, authority):
    """The generation number of a publication, or None when `previous` (number, sha256) already holds it. The same
    file published again later (an application's answer, published when it is read) is a new generation, so a list
    read again stays fresh."""
    digits = re.sub(r'\D', '', published)
    number = int(digits) if len(digits) == 14 else int(digits) * 1000000
    if previous is not None and previous[1] == sha256 and number <= previous[0]:
        return None
    if previous is not None and previous[0] >= number:
        if len(digits) == 8 and previous[0] // 1000000 == int(digits):
            return previous[0] + 1  # a second publication stamped with the same day
        raise SanctionsError(f'{authority}: the file is generation {number}, older than the imported {previous[0]}')
    return number


def previous_of(path):
    """(generation, source_sha256) of a generation this tool wrote, or None."""
    if not path.is_file():
        return None
    text = path.read_text(encoding='utf-8')
    number = re.search(r'^generation = (\d+)$', text, re.MULTILINE)
    digest = re.search(r'^source_sha256 = "([0-9a-f]{64})"$', text, re.MULTILINE)
    if not number or not digest:
        raise SanctionsError(f'{path}: not a generation {TOOL} wrote')
    return int(number.group(1)), digest.group(1)


def render(authority, config, jurisdiction, generation, published, sha256, parties, destinations=()):
    quote, strings = _rules.quote, _rules.strings
    origin = f'Authored in {REGISTER} from {config["url"]}, written by {TOOL}' if destinations else \
        f'Imported by {TOOL} from {config["url"]}'
    lines = [f'# {origin}; never edit by hand.',
             f'format = {quote(FORMAT)}',
             f'authority = {quote(authority)}',
             f'jurisdiction = {quote(jurisdiction)}',
             f'generation = {generation}',
             f'published = {quote(published[:10])}',
             f'max_age_days = {config["max_age_days"]}',
             f'source = {quote(config["url"])}',
             f'source_sha256 = {quote(sha256)}']
    for destination in destinations:
        lines += ['', '[[destination]]', f'region = {quote(destination["region"])}',
                  f'program = {quote(destination["program"])}', f'measure = {quote(destination["measure"])}']
    for party in parties:
        lines += ['', '[[party]]', f'id = {quote(party.id)}', f'kind = {quote(party.kind)}',
                  f'names = {strings(party.names)}', f'programs = {strings(party.programs)}',
                  f'listed = {quote(party.listed)}', f'births = {strings(party.births)}',
                  f'countries = {strings(party.countries)}', f'identifiers = {strings(party.identifiers)}']
    text = '\n'.join(lines) + '\n'
    if len(text.encode('utf-8')) > FILE_LIMIT:
        raise SanctionsError(f'{authority}: the generation is larger than {FILE_LIMIT} bytes')
    return text


def import_list(authority, config, jurisdiction, record, body, out_dir):
    """Writes OUT/<authority>.sanctions.toml from the fetched file and its fetch record; returns (generation or None
    when already current, parties, published)."""
    if len(body) > FILE_LIMIT:
        raise SanctionsError(f'{authority}: larger than {FILE_LIMIT} bytes')
    published, parties = READERS[config['format']].read(body, authority, record, config)
    if len(parties) > MAX_PARTIES:
        raise SanctionsError(f'{authority}: {len(parties)} parties, more than {MAX_PARTIES}')
    ids = [party.id for party in parties]
    if len(set(ids)) != len(ids):
        raise SanctionsError(f'{authority}: the party {next(i for i in ids if ids.count(i) > 1)} is listed twice')
    sha256 = hashlib.sha256(body).hexdigest()
    out = Path(out_dir) / f'{authority}{SUFFIX}'
    generation = generation_of(published, previous_of(out), sha256, authority)
    if generation is not None:
        out.parent.mkdir(parents=True, exist_ok=True)
        text = render(authority, config, jurisdiction, generation, published, sha256, parties)
        out.write_bytes(text.encode('utf-8'))
    return generation, len(parties), published


# Embargoed destinations ----------------------------------------------------------------------------------------------
#
# No designation file carries the destinations an authority embargoes: each authority's are authored in the register's
# sanctions_lists.embargoes from the instruments read (each entry's text, recorded by register-review.py read), one
# destination per region and programme with the provision it rests on and the entry it is read in, of the screening
# entry's jurisdiction: the screening entry itself when its instrument is one text (31 CFR chapter V), else the act the
# provision is in (an order the regulations incorporate, or one of the EU's Article 215 regulations, which no single
# text consolidates). The reading is those texts: the generation is published on the day of its latest one, so its
# maximum age runs from the reading; its source is the screening entry's primary and its source_sha256 the digest of
# the reading (every text's digest and the destinations), so a corrected reading of the same texts is a new
# generation.

EMBARGO_KEYS = {'entry', 'title', 'max_age_days', 'destinations', 'open'}
DESTINATION_KEYS = {'region', 'program', 'measure', 'basis', 'entry'}
# A destination's measure (crd::comply's Measure): comprehensive blocks a match; sectoral (some goods, services or
# parties restricted) and partial (part of the region, its boundary drawn by no subdivision) need review.
MEASURES = ('comprehensive', 'sectoral', 'partial')
REGION = re.compile(r'([A-Z]{2})(?:-[A-Z0-9]{1,3})?')  # crd::comply's is_region: ISO 3166-1, or a subdivision
MAX_DESTINATIONS = 1024  # kMaxDestinations
MAX_LIST_AGE_DAYS = 366  # kMaxListAgeDays


def entry_of(register, entry_id):
    return next((entry for entry in register.get('entries') or [] if entry.get('id') == entry_id), None)


def jurisdiction_of_entry(register, entry_id):
    """The jurisdiction of the entry's instrument, else None."""
    entry = entry_of(register, entry_id)
    instrument = entry.get('instrument') if isinstance(entry, dict) else None
    jurisdiction = instrument.get('jurisdiction') if isinstance(instrument, dict) else None
    return jurisdiction if isinstance(jurisdiction, str) and jurisdiction else None


def read_instrument(register, entry_id):
    """The entry's instrument when it was read from its primary source, else None."""
    entry = entry_of(register, entry_id)
    instrument = entry.get('instrument') if isinstance(entry, dict) else None
    if isinstance(instrument, dict) and instrument.get('consolidated') and (entry.get('source') or {}).get('verified'):
        return instrument
    return None


def embargo_errors(authority, record, register, today):
    """What keeps an authored embargo generation from being written on `today` (an ISO day): its shape, a text read
    from its primary source and in force (a measure renewed for a period rests on the renewal, whose sunset is the
    period's end), and destinations the decoder accepts (ISO regions of known countries, unique per programme, each
    with its provision)."""
    at = authority
    if not AUTHORITY.fullmatch(authority):
        return [f'{at}: an authority is a dotted lower-case id']
    if not isinstance(record, dict) or set(record) != EMBARGO_KEYS:
        return [f'{at} has {", ".join(sorted(EMBARGO_KEYS))}']
    errors = []
    jurisdiction = jurisdiction_of_entry(register, record['entry'])
    if jurisdiction is None:
        errors.append(f'{at}: entry {record["entry"]!r} is no entry of the register with an instrument and its '
                      'jurisdiction')
    if not isinstance(record['title'], str) or not record['title']:
        errors.append(f'{at}: title names the embargoes')
    if type(record['max_age_days']) is not int or not 1 <= record['max_age_days'] <= MAX_LIST_AGE_DAYS:
        errors.append(f"{at}: max_age_days is 1 to {MAX_LIST_AGE_DAYS} (the decoder's bound)")
    if record['open'] is not None and (not isinstance(record['open'], str) or not record['open']):
        errors.append(f'{at}: open is null or says what the reading still owes')
    destinations = record['destinations']
    if not isinstance(destinations, list) or not 1 <= len(destinations) <= MAX_DESTINATIONS:
        return errors + [f'{at}: destinations lists 1 to {MAX_DESTINATIONS} embargoed regions']
    countries, seen = set(COUNTRIES) - {None}, set()
    for destination in destinations:
        if not isinstance(destination, dict) or set(destination) != DESTINATION_KEYS or \
                not all(isinstance(destination[key], str) and destination[key] for key in DESTINATION_KEYS):
            errors.append(f'{at}: a destination has {", ".join(sorted(DESTINATION_KEYS))}, each a text')
            continue
        region = REGION.fullmatch(destination['region'])
        where = f'{at}: {destination["region"]}'
        if region is None or region.group(1) not in countries:
            errors.append(f'{where} is no ISO 3166-1 country or ISO 3166-2 subdivision of one {TOOL} knows')
        if destination['measure'] not in MEASURES:
            errors.append(f'{where}: measure is {", ".join(MEASURES[:-1])} or {MEASURES[-1]}')
        instrument = read_instrument(register, destination['entry'])
        if instrument is None:
            errors.append(f'{where}: entry {destination["entry"]!r} is not an instrument read from its primary source '
                          '(scripts/register-review.py read records it)')
        elif jurisdiction is not None and instrument.get('jurisdiction') != jurisdiction:
            errors.append(f'{where}: entry {destination["entry"]!r} is an instrument of '
                          f'{instrument.get("jurisdiction")}, not of {jurisdiction}')
        sunset = (entry_of(register, destination['entry']) or {}).get('sunset')
        if instrument is not None and isinstance(sunset, str) and sunset <= today:
            errors.append(f'{where}: entry {destination["entry"]!r} ended on {sunset}; read the resolution or act '
                          'that renews or replaces its measure, or drop the destination')
        if any(len(destination[key].encode('utf-8')) > MAX_TEXT for key in DESTINATION_KEYS):
            errors.append(f'{where}: a value longer than {MAX_TEXT} bytes')
        if (destination['region'], destination['program']) in seen:
            errors.append(f'{where} is embargoed twice under {destination["program"]}')
        seen.add((destination['region'], destination['program']))
    return errors


def embargo_texts(record, register):
    """{entry id: the text read} of every instrument a generation rests on: each destination's."""
    ids = sorted({destination['entry'] for destination in record['destinations']})
    instruments = {entry_id: read_instrument(register, entry_id) for entry_id in ids}
    return {entry_id: dict(instrument['consolidated'], url=instrument['primary'])
            for entry_id, instrument in instruments.items()}


def embargo_reading(record, register):
    """The SHA-256 of an authored generation's reading: the texts it was read from and its destinations."""
    reading = {'texts': embargo_texts(record, register), 'destinations': record['destinations']}
    canonical = json.dumps(reading, ensure_ascii=False, sort_keys=True, separators=(',', ':'))
    return hashlib.sha256(canonical.encode('utf-8')).hexdigest()


def import_embargo(authority, record, register, out_dir, today):
    """Writes OUT/<authority>.sanctions.toml as of `today` (an ISO day); returns (generation or None when already
    current, destinations, the day its texts were last read). A reading still open raises OpenReading."""
    errors = embargo_errors(authority, record, register, today)
    if errors:
        raise SanctionsError('; '.join(errors))
    if record['open'] is not None:
        raise OpenReading(f'{len(record["destinations"])} embargoed destinations authored, not written while the '
                          f'reading is open: {record["open"]}')
    instrument = entry_of(register, record['entry'])['instrument']
    published = max(text['read'] for text in embargo_texts(record, register).values())
    sha256 = embargo_reading(record, register)
    out = Path(out_dir) / f'{authority}{SUFFIX}'
    generation = generation_of(published, previous_of(out), sha256, authority)
    if generation is not None:
        out.parent.mkdir(parents=True, exist_ok=True)
        config = {'url': instrument['primary'], 'max_age_days': record['max_age_days']}
        text = render(authority, config, instrument['jurisdiction'], generation, published, sha256, [],
                      record['destinations'])
        out.write_bytes(text.encode('utf-8'))
    return generation, len(record['destinations']), published


# The register and the commands ---------------------------------------------------------------------------------------

def load_register(root):
    return json.loads((Path(root) / REGISTER).read_text(encoding='utf-8'))


def lists_of(register, chosen):
    lists = (register.get('sanctions_lists') or {}).get('lists') or {}
    unknown = sorted(set(chosen or []) - set(lists))
    if unknown:
        raise SanctionsError(f'{REGISTER} sanctions_lists has no {", ".join(unknown)}')
    return {authority: lists[authority] for authority in (chosen or lists)}


def jurisdiction_of(register, config):
    jurisdiction = jurisdiction_of_entry(register, config['entry'])
    if jurisdiction is None:
        raise SanctionsError(f'{REGISTER} has no entry {config["entry"]} with an instrument and its jurisdiction')
    return jurisdiction


def fetched(directory, authority, config):
    """The fetched file's body, checked against its fetch record."""
    path = Path(directory) / f'{authority}.{READERS[config["format"]].suffix}'
    record = json.loads(path.with_name(f'{authority}.fetch.json').read_text(encoding='utf-8'))
    body = path.read_bytes()
    if record.get('schema') != FETCH_SCHEMA or record.get('authority') != authority or \
            hashlib.sha256(body).hexdigest() != record.get('sha256') or len(body) != record.get('bytes'):
        raise SanctionsError(f'{path} is not the file its fetch record names; fetch it again')
    return record, body


def opener_for(config):
    """An https opener whose redirects stay on the official host or the delivery hosts the register names (OFAC
    and the UN answer with a signed link to their file store)."""
    spec = importlib.util.spec_from_file_location('register_review', ROOT / 'scripts/register-review.py')
    review = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(review)
    hosts = {urllib.parse.urlsplit(config['url']).hostname, *config['delivery']}
    return review, urllib.request.build_opener(review.SecureRedirects(hosts)).open


def download(config, url, post=None):
    """The body of `url` read through the list's opener, posted `post` as JSON when it is given."""
    review, opener = opener_for(config)
    headers, data = {}, None
    if post is not None:
        headers = {'Content-Type': 'application/json', 'Accept': 'application/json'}
        data = json.dumps(post).encode('utf-8')
    try:
        return review, review.download(opener, url, headers, FILE_LIMIT, data)
    except review.Unreadable as error:
        raise SanctionsError(f'{url} is {error}') from error


def command_fetch(args, root):
    register = load_register(root)
    into = Path(args.into)
    into.mkdir(parents=True, exist_ok=True)
    chosen, every = lists_of(register, args.authority), lists_of(register, None)
    for name in sorted({config['format'] for config in chosen.values()}):
        census = READERS[name].census
        configs = [config for config in every.values() if config['format'] == name]
        if census is not None:
            census(lambda url: download(configs[0], url)[1], configs)
    for authority, config in chosen.items():
        form = READERS[config['format']]
        review, body = download(config, config['url'], config['post'])
        now = datetime.datetime.now(datetime.timezone.utc)
        if form.canonical is not None:
            body = form.canonical(body, authority)
        sha256 = hashlib.sha256(body).hexdigest()
        (into / f'{authority}.{form.suffix}').write_bytes(body)
        record = {'schema': FETCH_SCHEMA, 'authority': authority, 'url': config['url'],
                  'day': now.date().isoformat(), 'instant': now.strftime('%Y-%m-%dT%H:%M:%SZ'), 'bytes': len(body),
                  'sha256': sha256}
        review.write_json(into / f'{authority}.fetch.json', record)
        read = config.get('read')
        state = 'the file the register recorded' if read and read['sha256'] == sha256 else \
            'a publication the register has not recorded'
        print(f'{authority}: {len(body)} bytes, SHA-256 {sha256}: {state}')
    return 0


def embargoes_of(register):
    return ((register.get('sanctions_lists') or {}).get('embargoes') or {}).get('generations') or {}


def command_import(args, root):
    register = load_register(root)
    lists, embargoes = lists_of(register, None), embargoes_of(register)
    chosen = args.authority or [*lists, *embargoes]
    unknown = sorted(set(chosen) - set(lists) - set(embargoes))
    if unknown:
        raise SanctionsError(f'{REGISTER} sanctions_lists has no {", ".join(unknown)}')
    today = datetime.datetime.now(datetime.timezone.utc).date().isoformat()
    for authority in dict.fromkeys(chosen):
        if authority in embargoes:
            try:
                generation, destinations, read = import_embargo(authority, embargoes[authority], register, args.out,
                                                                today)
            except OpenReading as reading:
                print(f'{authority}: {reading}')
                continue
            state = 'already written' if generation is None else f'generation {generation}'
            print(f'{authority}: {destinations} embargoed destinations read {read}: {state}')
            continue
        config = lists[authority]
        record, body = fetched(args.source, authority, config)
        generation, parties, published = import_list(authority, config, jurisdiction_of(register, config), record,
                                                     body, args.out)
        state = 'already imported' if generation is None else f'generation {generation}'
        print(f'{authority}: {parties} parties published {published}: {state}')
    return 0


def command_record(args, root):
    path = Path(root) / REGISTER
    register = load_register(root)
    for authority, config in lists_of(register, args.authority).items():
        record, body = fetched(args.source, authority, config)
        published, parties = READERS[config['format']].read(body, authority, record, config)
        config['read'] = {'day': record['day'], 'published': published, 'bytes': record['bytes'],
                          'sha256': record['sha256'], 'parties': len(parties)}
        print(f'{authority}: recorded the file read on {record["day"]} ({len(parties)} parties, published '
              f'{published})')
    path.write_text(json.dumps(register, indent=2, ensure_ascii=False) + '\n', encoding='utf-8', newline='\n')
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n', 1)[0])
    parser.add_argument('--root', default=str(ROOT))
    commands = parser.add_subparsers(dest='command', required=True)
    fetch = commands.add_parser('fetch', help='download the official files')
    fetch.add_argument('--into', required=True)
    imported = commands.add_parser('import', help='write the cerid-sanctions/1 generations')
    imported.add_argument('--from', dest='source', required=True)
    imported.add_argument('--out', required=True)
    record = commands.add_parser('record', help='record the fetched files as the lists read')
    record.add_argument('--from', dest='source', required=True)
    for command in (fetch, imported, record):
        command.add_argument('--authority', action='append')
    args = parser.parse_args(argv)
    handlers = {'fetch': command_fetch, 'import': command_import, 'record': command_record}
    try:
        return handlers[args.command](args, Path(args.root))
    except (SanctionsError, OSError, http.client.HTTPException, ValueError) as error:
        print(f'import-sanctions: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
