#!/usr/bin/env python3.8

with open('js_fragments.cpp', 'w') as out:
    out.write('#include <Arduino.h>\n\n')
    out.write('const String js_log_script =\n')
    with open('static/log_script.js', 'r') as inp:
        for line in inp:
            assert line.endswith('\n')
            line = line[:-1]
            if not line.strip():
                continue
            stripped_line = line.strip()
            if stripped_line.startswith('//'):
                continue
            indentation = ' ' * (len(line) - len(stripped_line))
            line = stripped_line
            line = line.replace(' {', '{')
            line = line.replace(', ', ',')
            line = line.replace('\\', r'\\')
            line = line.replace('"', r'\"')
            out.write(f'  {indentation}"{line}\\n"\n')
    out.write('  ;\n')
