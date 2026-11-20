# Jackcess 5.2.0 empty database templates

These two binary templates are copied from the `jackcess-5.2.0` tag at commit
`6159c680933625ffa952334b5e0e6b24f37bfdfe` in
[`spannm/jackcess`](https://github.com/spannm/jackcess):

| Local file | Upstream resource | Size | SHA-256 |
|---|---|---:|---|
| `empty-v2000.mdb` | `src/main/resources/io/github/spannm/jackcess/empty.mdb` | 98,304 bytes | `521c6f8a60f94273866367f69c772eb590fc7fcb8bc91bcb134d567b1e9b6566` |
| `empty-v2003.mdb` | `src/main/resources/io/github/spannm/jackcess/empty2003.mdb` | 204,800 bytes | `c7dc939c0afb5706d209ea54cfd1dfc7e67996767a0ed357c0fd981b9012c955` |

The upstream project distributes these resources under the Apache License 2.0;
the license text is included as `LICENSE.txt`. CMake embeds the files into the
core static library, so database creation has no runtime dependency on the
Jackcess JAR or on external template paths.
