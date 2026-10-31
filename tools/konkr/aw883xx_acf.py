#!/usr/bin/env python3
"""Parse an Awinic AW883xx ACF container (vendor/firmware/aw883xx_acf.bin) and list what it
holds per device/profile; with --emit, write a C table of the register profile and the DSP
firmware/config blobs for the LK (tools/konkr -> platform/mt6785/ayaneo_aw883xx_acf.h).
Layout follows aw_data_type.h / aw_bin_parse.c of the Awinic kernel driver."""
import struct, sys

SEC = {0:'REG',1:'DSP',2:'DSP_CFG',3:'DSP_FW',4:'HDR_REG',5:'HDR_DSP_CFG',6:'HDR_DSP_FW',7:'MULTIBIN',8:'SKT_PROJECT',9:'DSP_PROJECT',10:'MONITOR'}
PROF = ['Music','Voice','Voip','Ringtone','Ringtone_hs','Lowpower','Bypass','Mmi','Fm','Notification','Receiver']

def parse(path):
    b = open(path,'rb').read()
    a_id, proj, cust, ver, author, ddt_size, ddt_num, hdr_off, hdr_ver = struct.unpack_from('<I24s16s4sIIIII', b, 0)
    assert a_id == 0xa15f908, hex(a_id)
    print('project %r customer %r version %s author %u ddt size %u num %u off %u hdr_ver 0x%08x len %u' %
          (proj.rstrip(b'\0'), cust.rstrip(b'\0'), ver.hex(), author, ddt_size, ddt_num, hdr_off, hdr_ver, len(b)))
    entries = []
    for i in range(ddt_num):
        o = hdr_off + i*(64 if hdr_ver == 1 else 108)   # a_ddt_size is the WHOLE table
        typ, name, dev_index, dev_bus, dev_addr, dev_prof, dtype, dsize, doff, dcrc = struct.unpack_from('<I16sHHHHIIII', b, o)
        pstr = ''; chip = 0
        if hdr_ver == 0x01000000:
            pstr = struct.unpack_from('<32s', b, o+44)[0].rstrip(b'\0').decode(errors='replace')
            chip = struct.unpack_from('<I', b, o+76)[0]
        e = dict(type=typ, name=name.rstrip(b'\0').decode(errors='replace'), dev_index=dev_index, bus=dev_bus,
                 addr=dev_addr, prof=dev_prof, pstr=pstr, dtype=dtype, size=dsize, off=doff, crc=dcrc, chip=chip)
        entries.append(e)
        print(' dde %2d type %u name %-10s idx %u bus %u addr 0x%02x prof %u %-10s data %-11s size %6u off %6u chip 0x%x' %
              (i, typ, e['name'], dev_index, dev_bus, dev_addr, dev_prof, pstr or PROF[dev_prof] if dev_prof < len(PROF) else '?', SEC.get(dtype,'?'), dsize, doff, chip))
    return b, entries

BINTYPE = {0x0:'REGISTER',0x10:'DSP_REG',0x11:'DSP_CFG',0x20:'SOC_REG',0x21:'DSP_FW',0x2000:'MULTI'}
def subbins(d):
    """Walk an Awinic multi-bin: outer 60 byte header, count at +60, the first sub-bin at
    60 + start address, each sub-bin = 60 byte header + bin_data_len (aw_bin_parse.c)."""
    hdr = struct.unpack_from('<5I', d, 0)   # check_sum, header_ver, type, data_ver, data_len (header_len is implicit 60)
    assert hdr[2] == 0x2000, hex(hdr[2])
    n = struct.unpack_from('<I', d, 60)[0]
    start = struct.unpack_from('<I', d, 64)[0]
    p = 60 + start
    out = []
    for i in range(n):
        check_sum, header_ver, btype, bver, blen = struct.unpack_from('<5I', d, p)
        ui_ver = struct.unpack_from('<I', d, p+20)[0]
        chip = d[p+24:p+32].rstrip(b'\0')
        reg_byte_len, data_byte_len, device_addr = struct.unpack_from('<3I', d, p+32)
        payload = d[p+60:p+60+blen]
        out.append(dict(type=btype, len=blen, chip=chip, reg_byte_len=reg_byte_len, data_byte_len=data_byte_len, device_addr=device_addr, payload=payload, ver=bver))
        p += 60 + blen
    return out

def section_payload(b, e):
    """raw sections are the data as-is; HDR_* sections carry a 60 byte Awinic bin header."""
    d = b[e['off']:e['off']+e['size']]
    if e['dtype'] in (4,5,6):
        check_sum, header_ver, bin_data_type, bin_data_ver, bin_data_len = struct.unpack_from('<5I', d, 0)
        return d[60:60+bin_data_len]
    return d

def regs_of(payload):
    """register list: little endian 16 bit pairs (addr, value), as aw_dev_reg_container_update reads them"""
    n = len(payload)//4
    return [struct.unpack_from('<HH', payload, 4*i) for i in range(n)]

if __name__ == '__main__':
    b, entries = parse(sys.argv[1])
    for e in entries:
        if e['dtype'] == 7:
            for k, sb in enumerate(subbins(b[e['off']:e['off']+e['size']])):
                print(' sub %d: type 0x%x %-8s len %6u chip %r reg_byte %u data_byte %u dev_addr 0x%x ver 0x%x' % (k, sb['type'], BINTYPE.get(sb['type'],'?'), sb['len'], sb['chip'], sb['reg_byte_len'], sb['data_byte_len'], sb['device_addr'], sb['ver']))
                if sb['type'] == 0:
                    cnt = struct.unpack_from('<I', sb['payload'], 0)[0]
                    regs = regs_of(sb['payload'][4:4+cnt*4])
                    print('   %u regs: ' % cnt + ' '.join('%02x=%04x' % r for r in regs))
                elif sb['type'] in (0x10, 0x21):
                    print('   prefix: ' + sb['payload'][:12].hex() + ' ... first data: ' + sb['payload'][12:28].hex())
        if e['dtype'] in (0,4):
            regs = regs_of(section_payload(b, e))
            print(' REG dev %u addr 0x%02x prof %u: %u regs: ' % (e['dev_index'], e['addr'], e['prof'], len(regs)) +
                  ' '.join('%02x=%04x' % r for r in regs))
    if '--emit' in sys.argv:
        out = sys.argv[sys.argv.index('--emit')+1]
        with open(out,'w') as f:
            f.write('/* Generated by tools/konkr/aw883xx_acf.py from the stock vendor/firmware/aw883xx_acf.bin\n'
                    ' * (project A2066, AW88166): the Music profile per amp as the driver streams it. The\n'
                    ' * register table is (addr, value) pairs; the DSP firmware and config blobs are the sub-bin\n'
                    ' * payloads after their 12 byte prefix, written verbatim through DSPMADD/DSPMDAT at the\n'
                    ' * chip bases 0x8980 (fw) and 0x9B00 (cfg). */\n')
            for e in entries:
                if e['prof'] != 0 or e['dtype'] != 7: continue
                for sb in subbins(b[e['off']:e['off']+e['size']]):
                    dev = e['dev_index']
                    if sb['type'] == 0:
                        cnt = struct.unpack_from('<I', sb['payload'], 0)[0]
                        regs = regs_of(sb['payload'][4:4+cnt*4])
                        f.write('static const unsigned short aw88166_reg_dev%u[%u][2] = {\n' % (dev, len(regs)))
                        for i in range(0, len(regs), 6):
                            f.write('\t' + ' '.join('{0x%02x,0x%04x},' % r for r in regs[i:i+6]) + '\n')
                        f.write('};\n')
                    elif sb['type'] in (0x10, 0x21):
                        name = 'aw88166_%s_dev%u' % ('dspcfg' if sb['type'] == 0x10 else 'dspfw', dev)
                        p = sb['payload'][12:]
                        f.write('static const unsigned char %s[%u] = {\n' % (name, len(p)))
                        for i in range(0, len(p), 16):
                            f.write('\t' + ','.join('0x%02x' % x for x in p[i:i+16]) + ',\n')
                        f.write('};\n')
        print('wrote', out)
