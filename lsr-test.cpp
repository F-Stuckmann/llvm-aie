// Strided loads: on aie2ps LSR folds the 2*i index into its own stride-2 IV.
// A plain dst[i] = src[i] loop is left untouched by LSR on this target.
void scale(int *dst, const int *src, unsigned n) {
  for (unsigned i = 0; i < n; ++i)
    dst[i] = src[2 * i] * 3 + src[2 * i + 1];
}
