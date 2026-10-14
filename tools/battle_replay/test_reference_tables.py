"""Regression for the official base-version package plus emergency overrides."""
import struct
import unittest

from fetch_reference import flash_versions


def version_package(xml):
    def tag(code, data):
        return struct.pack('<HI', (code << 6) | 63, len(data)) + data
    symbols = struct.pack('<HH', 1, 1) + b'mmo.versiondata.DocClass_cls\0'
    blob = struct.pack('<HI', 1, 0) + xml
    body = b'\x08\x00\x00\x18\x01\x00' + tag(76, symbols) + tag(87, blob) + b'\0\0'
    return b'FWS\x09' + struct.pack('<I', len(body) + 8) + body


class VersionTableTests(unittest.TestCase):
    def test_start_version_does_not_replace_base_resource_versions(self):
        manifest = b'<r><v>99999</v><u><f n="battle/battle" v="300"/></u></r>'
        package = version_package(b'<version><f n="library/common" v="100"/><f n="battle/battle" v="200"/></version>')
        versions = flash_versions(manifest, package)
        self.assertEqual(versions, {'library/common': '100', 'battle/battle': '300'})
        self.assertNotIn('battlebuffer/buffer645', versions)


if __name__ == '__main__':
    unittest.main()
