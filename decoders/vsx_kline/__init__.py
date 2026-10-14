##
## This file is part of VimuScopeX.
##
## This program is free software; you can redistribute it and/or modify
## it under the terms of the GNU General Public License as published by
## the Free Software Foundation; either version 2 of the License, or
## (at your option) any later version.
##

'''
K-Line (ISO 9141-2 / ISO 14230 KWP2000) message framing, stacked on the
'uart' decoder (10400 baud, 8N1 on the K line).

KWP2000 framing reads the format byte: its top two bits give the address
mode (target/source bytes present or not), its low six bits the data
length (0 means a separate length byte follows). The frame ends with an
8-bit sum checksum, which is verified.

Gap framing (ISO 9141-2) has no length field: a message ends at an idle
gap longer than the configured number of bit times, and its last byte is
checked as the sum checksum of the bytes before it.
'''

from .pd import Decoder
