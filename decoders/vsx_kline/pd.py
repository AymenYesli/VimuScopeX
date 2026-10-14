##
## This file is part of VimuScopeX.
##
## This program is free software; you can redistribute it and/or modify
## it under the terms of the GNU General Public License as published by
## the Free Software Foundation; either version 2 of the License, or
## (at your option) any later version.
##

import sigrokdecode as srd

# A few common KWP2000 / OBD service IDs, for readable labels. Positive
# responses are the request ID + 0x40; 0x7F is a negative response.
services = {
    0x01: 'OBD mode 01', 0x02: 'OBD mode 02', 0x03: 'OBD mode 03',
    0x04: 'OBD mode 04', 0x09: 'OBD mode 09',
    0x10: 'StartDiagSession', 0x11: 'ECUReset', 0x14: 'ClearDTC',
    0x17: 'ReadDTCStatus', 0x18: 'ReadDTCByStatus', 0x1A: 'ReadECUId',
    0x21: 'ReadDataByLocalId', 0x22: 'ReadDataById', 0x23: 'ReadMemory',
    0x27: 'SecurityAccess', 0x2E: 'WriteDataById', 0x30: 'IOControl',
    0x31: 'StartRoutine', 0x3B: 'WriteDataByLocalId', 0x3E: 'TesterPresent',
    0x7F: 'NegativeResponse', 0x81: 'StartComm', 0x82: 'StopComm',
    0x83: 'AccessTiming',
}

def service_name(sid):
    if sid in services:
        return services[sid]
    if sid >= 0x40 and (sid - 0x40) in services:
        return services[sid - 0x40] + ' +'
    return None

class Ann:
    FMT, TGT, SRC, LEN, SID, DATA, CS, ERROR = range(8)

class Decoder(srd.Decoder):
    api_version = 3
    id = 'vsx_kline'
    name = 'K-Line'
    longname = 'K-Line (ISO 9141-2 / ISO 14230 KWP2000)'
    desc = 'Automotive diagnostic K-Line message framing.'
    license = 'gplv2+'
    inputs = ['uart']
    outputs = ['kline']
    tags = ['Automotive']
    options = (
        {'id': 'framing', 'desc': 'Framing', 'default': 'kwp2000',
            'values': ('kwp2000', 'gap')},
        {'id': 'gap_bits', 'desc': 'Inter-message gap (bit times)',
            'default': 260},
    )
    annotations = (
        ('fmt', 'Format byte'),
        ('tgt', 'Target address'),
        ('src', 'Source address'),
        ('len', 'Length byte'),
        ('sid', 'Service ID'),
        ('data', 'Data byte'),
        ('cs', 'Checksum'),
        ('error', 'Error'),
    )
    annotation_rows = (
        ('fields', 'Fields', (Ann.FMT, Ann.TGT, Ann.SRC, Ann.LEN, Ann.SID,
                              Ann.DATA, Ann.CS)),
        ('errors', 'Errors', (Ann.ERROR,)),
    )

    def __init__(self):
        self.reset()

    def reset(self):
        self.bytes = []          # (ss, es, value) of the message so far
        self.last_es = None
        self.bit = None          # bit time in samples, from the UART bytes

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)

    def putb(self, b, cls, texts):
        self.put(b[0], b[1], self.out_ann, [cls, texts])

    def put_error(self, ss, es, texts):
        self.put(ss, es, self.out_ann, [Ann.ERROR, texts])

    # --- KWP2000: framing from the format / length bytes -------------------

    def kwp_try_complete(self):
        b = self.bytes
        fmt = b[0][2]
        has_addr = (fmt & 0xC0) != 0
        header = 1 + (2 if has_addr else 0)
        length = fmt & 0x3F
        len_byte = length == 0
        if len_byte:
            if len(b) < header + 1:
                return
            length = b[header][2]
            header += 1
        total = header + length + 1
        if len(b) < total:
            return

        self.putb(b[0], Ann.FMT, ['Format %02X' % fmt, 'Fmt %02X' % fmt, '%02X' % fmt])
        i = 1
        if has_addr:
            self.putb(b[1], Ann.TGT, ['Target %02X' % b[1][2], 'Tgt %02X' % b[1][2], '%02X' % b[1][2]])
            self.putb(b[2], Ann.SRC, ['Source %02X' % b[2][2], 'Src %02X' % b[2][2], '%02X' % b[2][2]])
            i = 3
        if len_byte:
            self.putb(b[i], Ann.LEN, ['Length %d' % length, 'Len %d' % length, '%d' % length])
            i += 1
        for k in range(length):
            byte = b[i + k]
            v = byte[2]
            if k == 0:
                name = service_name(v)
                texts = ['SID %02X' % v, '%02X' % v]
                if name:
                    texts.insert(0, '%s (%02X)' % (name, v))
                self.putb(byte, Ann.SID, texts)
            else:
                self.putb(byte, Ann.DATA, ['%02X' % v])
        cs_byte = b[total - 1]
        self.check_sum([x[2] for x in b[:total - 1]], cs_byte)
        self.bytes = b[total:]

    # --- Gap framing (ISO 9141-2): message ends at an idle gap -------------

    def gap_flush(self):
        b = self.bytes
        if not b:
            return
        if len(b) == 1:
            self.putb(b[0], Ann.DATA, ['%02X' % b[0][2]])
        else:
            self.check_sum([x[2] for x in b[:-1]], b[-1])
        self.bytes = []

    def check_sum(self, values, cs_byte):
        expected = sum(values) & 0xFF
        got = cs_byte[2]
        if expected == got:
            self.putb(cs_byte, Ann.CS, ['Checksum %02X OK' % got, 'CS OK', 'OK'])
        else:
            self.put_error(cs_byte[0], cs_byte[1],
                ['Checksum %02X, expected %02X' % (got, expected), 'CS ERR', 'ERR'])

    def decode(self, ss, es, data):
        ptype, rxtx, pdata = data
        if ptype != 'DATA' or rxtx != 0:
            return
        value = pdata[0]
        bit = max(1.0, (es - ss) / 8.0)
        self.bit = bit

        gap = self.last_es is not None and \
            (ss - self.last_es) > self.options['gap_bits'] * bit
        if gap and self.bytes:
            if self.options['framing'] == 'gap':
                self.gap_flush()
            else:
                # A KWP2000 message cut short by an idle gap.
                self.put_error(self.bytes[0][0], self.bytes[-1][1],
                    ['Incomplete message', 'Incomplete', '!'])
                self.bytes = []
        self.last_es = es

        if self.options['framing'] == 'gap':
            # Every byte but the last is plain data; the last one is only
            # known to be the checksum once the gap after it arrives.
            if self.bytes:
                prev = self.bytes[-1]
                self.putb(prev, Ann.DATA, ['%02X' % prev[2]])
            self.bytes.append((ss, es, value))
            return

        self.bytes.append((ss, es, value))
        self.kwp_try_complete()
