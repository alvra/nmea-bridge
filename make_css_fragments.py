#!/usr/bin/env python3.8

with open('css_fragments.cpp', 'w') as out:
    out.write('#include <Arduino.h>\n\n')
    out.write('const String css_style =\n')
    with open('static/style.css', 'r') as inp:
        for line in inp:
            assert line.endswith('\n')
            line = line[:-1]
            if line.endswith(' {'):
                line = line[:-2] + '{'
            stripped_line = line.strip()
            indentation = ' ' * (len(line) - len(stripped_line))
            line = stripped_line
            line = line.replace(': ', ':')
            line = line.replace(', ', ',')
            line = line.replace('"', r'\"')
            # TODO remove last ; of each block
            out.write(f'  {indentation}"{line}"\n')
    out.write('  ;\n')
