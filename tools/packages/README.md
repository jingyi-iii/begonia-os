# packages/

Source tarballs for the cross toolchain. **They are not committed to git:**

- `gcc-16.2.0.tar.xz` is 107,200,820 bytes — over GitHub's 100 MiB per-file limit,
  so a push containing it is rejected outright;
- the full set would add ~162 MB to every clone, for files that are byte-identical
  to the upstream releases anyway.

Restore them with:

```bash
bash tools/fetch-packages.sh           # download whatever is missing (verifies sha256)
bash tools/fetch-packages.sh --check   # verify only, download nothing
```

## Expected files

| file | bytes | sha256 (first 16) |
| --- | ---: | --- |
| `gmp-6.3.0.tar.xz` | 2,094,196 | `a3c2b80201b89e68` |
| `mpfr-4.2.2.tar.xz` | 1,505,596 | `b67ba0383ef7e8a8` |
| `mpc-1.3.1.tar.gz` | 773,573 | `ab642492f5cf882b` |
| `isl-0.24.tar.bz2` | 2,261,594 | `fcf78dd9656c10eb` |
| `zstd-1.5.7.tar.gz` | 2,434,947 | `eb33e51f49a15e02` |
| `binutils-2.46.0.tar.xz` | 28,548,776 | `d75a94f4d73e7a40` |
| `gdb-17.2.tar.xz` | 24,658,624 | `1c036c0d72e4b3d1` |
| `gcc-16.2.0.tar.xz` | 107,200,820 | `e6738e29597f7332` |

The full hashes and the mirror list live in
[`../fetch-packages.sh`](../fetch-packages.sh) — that is the single source of truth.

## Adding or upgrading a package

1. drop the tarball here, named `name-version.tar.<ext>` (`build-toolchain.sh`
   finds components by glob, e.g. `gcc-*.tar.xz`);
2. add its `file|sha256|mirror` line to `../fetch-packages.sh`;
3. update the table above.

Keep the versions in sync with GCC's `contrib/download_prerequisites`
(currently gmp 6.3.0, mpfr 4.2.2, mpc 1.3.1, isl 0.24).
