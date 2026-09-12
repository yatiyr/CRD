"""Canonical spelling for the FFT generators' restricted C++ declaration grammar.

This preserves expression order and fused kernel scope. It is not a C++ formatter.
"""
import re


_DECLARATION = re.compile(
    r'(?P<prefix>^[ \t]*(?:\{[ \t]*)?)'
    r'(?P<type>(?:const )?(?:T|V|crd::usize|crd::f32|crd::f64)) '
    r'(?P<body>[A-Za-z_]\w*\s*(?:=|,)[^;\n]*);', re.MULTILINE)


def canonicalize(source):
    """Split only known generated value declarations; retain commas inside expressions."""
    def split(match):
        body = match['body']
        depth = 0
        start = 0
        parts = []
        for index, char in enumerate(body):
            if char in '([{':
                depth += 1
            elif char in ')]}':
                depth -= 1
                if depth < 0:
                    raise ValueError('Unbalanced generated declaration')
            elif char == ',' and depth == 0:
                parts.append(body[start:index].strip())
                start = index + 1
        if depth:
            raise ValueError('Unbalanced generated declaration')
        parts.append(body[start:].strip())
        if len(parts) == 1:
            return match[0]
        if not all(re.match(r'^[A-Za-z_]\w*(?:\s*=.*)?$', part) for part in parts):
            raise ValueError('Unsupported generated declaration')
        indent = re.match(r'[ \t]*', match['prefix'])[0]
        if '{' in match['prefix']:
            indent += '  '
        return match['prefix'] + ('\n' + indent).join(match['type'] + ' ' + part + ';' for part in parts)

    source = _DECLARATION.sub(split, source)
    source = re.sub(
        r'^(?P<indent>[ \t]*)(?P<type>const crd::f(?:32|64)\* const) tr = ([^;,\n]+), \* const ti = ([^;,\n]+);',
        lambda match: (match['indent'] + match['type'] + ' tr = ' + match[3] + ';\n'
                       + match['indent'] + match['type'] + ' ti = ' + match[4] + ';'),
        source, flags=re.MULTILINE)
    for old, new in {'hi_': 'high_im', 'or_': 'out_re', 'oi_': 'out_im'}.items():
        source = re.sub(r'\b' + old + r'\b', new, source)
    # One generated DAG must remain one scheduled scope. Keep all other checks active.
    annotation = '// NOLINTNEXTLINE(readability-function-size) -- Generated scheduled DAG; retain fused scope.\n'
    source = source.replace(annotation, '')
    return re.sub(r'^(CRD_FFT_(?:GEN|CODELET)_INLINE void )', annotation + r'\1', source, flags=re.MULTILINE)
