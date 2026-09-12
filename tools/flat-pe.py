"""Verify actual native PE import closure; never alter executable bytes."""
import argparse
import hashlib
from pathlib import Path
import shutil
import struct

MACHINES = {'arm64': 0xaa64, 'x64': 0x8664, 'x86': 0x14c}
SYSTEM = {'kernel32.dll', 'user32.dll', 'gdi32.dll', 'advapi32.dll', 'ole32.dll',
          'oleaut32.dll', 'shell32.dll', 'shlwapi.dll', 'cfgmgr32.dll', 'ntdll.dll',
          'bcrypt.dll', 'version.dll', 'ws2_32.dll', 'secur32.dll', 'rpcrt4.dll',
          'msvcrt.dll', 'ucrtbase.dll', 'psapi.dll', 'setupapi.dll'}

def pe(path):
    data = path.read_bytes()
    if data[:2] != b'MZ': raise ValueError(f'Not PE: {path}')
    start, = struct.unpack_from('<I', data, 0x3c)
    if data[start:start+4] != b'PE\0\0': raise ValueError(f'Bad PE: {path}')
    machine, sections = struct.unpack_from('<HH', data, start+4)
    optional_size, = struct.unpack_from('<H', data, start+20)
    optional = start+24
    magic, = struct.unpack_from('<H', data, optional)
    directories = optional + (112 if magic == 0x20b else 96)
    def offset(rva):
        for i in range(sections):
            size, address, raw_size, raw = struct.unpack_from('<IIII', data, optional+optional_size+i*40+8)
            if address <= rva < address+max(size, raw_size): return raw+rva-address
        raise ValueError(f'Invalid RVA: {path}')
    imported = []
    for directory, width, namefield in ((1,20,12),(13,32,4)):
        rva, = struct.unpack_from('<I', data, directories+directory*8)
        if not rva: continue
        cursor = offset(rva)
        while any(data[cursor:cursor+width]):
            if directory == 13 and struct.unpack_from('<I', data, cursor)[0] != 1:
                raise ValueError(f'Unsupported VA delay import: {path}')
            name_rva, = struct.unpack_from('<I', data, cursor+namefield)
            name = offset(name_rva)
            imported.append(data[name:data.index(0,name)].decode('ascii').lower())
            cursor += width
    return machine, imported

def system(name):
    return name in SYSTEM or name.startswith(('api-ms-win-', 'ext-ms-win-'))

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler', type=Path)
    parser.add_argument('--crt', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--arch', choices=MACHINES)
    args = parser.parse_args()
    if args.compiler:
        args.output.mkdir(exist_ok=False)
        shutil.copy2(args.compiler, args.output/'viogpu_clspv_x64.exe')
        available = {p.name.lower():p for p in args.crt.glob('*.dll')}
        pending = [args.output/'viogpu_clspv_x64.exe']
        while pending:
            item = pending.pop()
            machine, imports = pe(item)
            if machine != MACHINES['x64']: raise ValueError(f'Not x64 compiler closure: {item}')
            for name in imports:
                if system(name): continue
                target = args.output/name
                if target.exists(): continue
                if name not in available: raise ValueError(f'Missing compiler dependency: {name}')
                shutil.copy2(available[name], target)
                pending.append(target)
    else:
        for item in args.output.iterdir():
            if item.suffix.lower() not in ('.dll','.exe'): continue
            machine, imports = pe(item)
            if machine != MACHINES[args.arch]: raise ValueError(f'Wrong {args.arch} machine: {item}')
            for name in imports:
                if not system(name) and not (args.output/name).exists():
                    raise ValueError(f'Missing private import: {item.name} -> {name}')
                if name.startswith(('vcruntime','msvcp','concrt')):
                    raise ValueError(f'Runtime must use static CRT: {item.name} -> {name}')
    print('PASS PE architecture and complete flat import closure')

if __name__ == '__main__': main()
