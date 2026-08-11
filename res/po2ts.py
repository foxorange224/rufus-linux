#!/usr/bin/env python3
"""Convert Rufus PO translation files to Qt TS format."""

import os
import re
import sys
import xml.etree.ElementTree as ET
from xml.dom import minidom

PO_DIR = os.path.join(os.path.dirname(__file__), '..', 'rufus-original', 'res', 'loc', 'po')
TS_DIR = os.path.join(os.path.dirname(__file__), 'translations')

def lang_to_qt(lang):
    """Convert POSIX locale (es_ES) to Qt format (es)."""
    parts = lang.split('_')
    if len(parts) >= 2 and parts[0].lower() == parts[1].lower():
        return parts[0].lower()
    return lang.replace('_', '-')

def parse_po(path):
    """Parse a PO file and return list of (msgid, msgstr, context) tuples."""
    entries = []
    current_id = None
    current_str = None
    current_context = None
    
    with open(path, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.rstrip('\n')
            
            # Context comment: #. • IDD_DIALOG → IDS_DRIVE_PROPERTIES_TXT
            m = re.match(r'#\.\s*•\s*(\S+)', line)
            if m:
                current_context = m.group(1)
            
            m = re.match(r'msgid\s+"(.*)"', line)
            if m:
                if current_id is not None and current_str is not None:
                    entries.append((current_id, current_str, current_context))
                current_id = m.group(1)
                current_str = None
                continue
            
            m = re.match(r'msgstr\s+"(.*)"', line)
            if m:
                current_str = m.group(1)
                continue
            
            # Multi-line msgid
            if current_id is not None and current_str is None:
                m = re.match(r'"(.+)"', line)
                if m:
                    current_id += m.group(1)
            
            # Multi-line msgstr
            if current_str is not None:
                m = re.match(r'"(.+)"', line)
                if m:
                    current_str += m.group(1)
    
    if current_id is not None and current_str is not None:
        entries.append((current_id, current_str, current_context))
    
    return entries

def escape_xml(s):
    """Escape string for XML."""
    s = s.replace('&', '&amp;')
    s = s.replace('<', '&lt;')
    s = s.replace('>', '&gt;')
    s = s.replace('"', '&quot;')
    s = s.replace("'", '&apos;')
    s = s.replace('\n', '&#10;')
    return s

def create_ts(lang, entries):
    """Create a Qt .ts file as an XML string."""
    lines = []
    lines.append('<?xml version="1.0" encoding="utf-8"?>')
    lines.append('<!DOCTYPE TS>')
    qt_lang = lang_to_qt(lang)
    lines.append(f'<TS version="2.1" language="{qt_lang}">')
    
    # Group by context
    contexts = {}
    for msgid, msgstr, context in entries:
        if not msgstr:
            continue
        ctx = context or 'MainWindow'
        if ctx not in contexts:
            contexts[ctx] = []
        contexts[ctx].append((msgid, msgstr))
    
    for ctx_name in sorted(contexts.keys()):
        lines.append('  <context>')
        lines.append(f'    <name>{escape_xml(ctx_name)}</name>')
        for msgid, msgstr in contexts[ctx_name]:
            lines.append('    <message>')
            lines.append(f'      <source>{escape_xml(msgid)}</source>')
            lines.append(f'      <translation>{escape_xml(msgstr)}</translation>')
            lines.append('    </message>')
        lines.append('  </context>')
    
    lines.append('</TS>')
    return '\n'.join(lines)

def main():
    os.makedirs(TS_DIR, exist_ok=True)
    
    po_files = [f for f in os.listdir(PO_DIR) if f.endswith('.po')]
    
    for po_file in sorted(po_files):
        lang = po_file[:-3]  # Remove .po extension
        po_path = os.path.join(PO_DIR, po_file)
        entries = parse_po(po_path)
        
        if not entries:
            print(f"  No translations found in {po_file}")
            continue
        
        ts_content = create_ts(lang, entries)
        
        ts_name = f"rufus_{lang_to_qt(lang)}.ts"
        ts_path = os.path.join(TS_DIR, ts_name)
        
        with open(ts_path, 'w', encoding='utf-8') as f:
            f.write(ts_content)
        
        print(f"  {po_file} -> {ts_name}: {sum(1 for _, s, _ in entries if s)} translations")
    
    # Also create a rufus_en.ts that maps language codes
    ts_path = os.path.join(TS_DIR, 'rufus_en.ts')
    with open(ts_path, 'w', encoding='utf-8') as f:
        f.write('<?xml version="1.0" encoding="utf-8"?>\n')
        f.write('<!DOCTYPE TS>\n')
        f.write('<TS version="2.1" language="en">\n')
        f.write('</TS>\n')
    print(f"  Created rufus_en.ts (empty base)")

if __name__ == '__main__':
    main()
