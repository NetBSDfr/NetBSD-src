import re
import sys

keywords = ["defined", "sizeof", "return", "while", "panic"]
multiline = [ "calc_cache_size", "fork1", "ctob"]

with open(sys.argv[1], 'r') as file:
   lines = file.readlines()

in_section = False
exit_func = None
for line in lines:
    if exit_func:
        print(line.rstrip())
        print(exit_func)
        exit_func = None
        continue
    elif 'TSENTER()' in line:
        in_section = True
        print(line.rstrip())
    elif 'TSEXIT()' in line:
        in_section = False
        print(line.rstrip())
    elif in_section and re.match(r'[^\w]+\s*/?\*+/?\s*.*', line):
        print(line.rstrip())
    elif in_section and any(k in line for k in keywords):
        print(line.rstrip())
    elif in_section and re.match(r'.+[a-z0-9_]+\([^\)]*\)', line):
        m = re.findall(r'(\s*)([a-z0-9_]+)\([^\)]*\)', line)
        if m:
            s, f = m[0]
            print(f'{s}TSENTER2("{f}");')
            print(line.rstrip())
            exit_func = f'{s}TSEXIT2("{f}");\n'
            if any(l in line for l in multiline):
                continue;
            print(exit_func)
            exit_func = None
        else:
            print(line.rstrip())
    else:
        print(line.rstrip())
