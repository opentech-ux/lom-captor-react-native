/** Hash logical route segments without retaining or logging the raw name. */
export const anonymousRoute = (name: string): string[] => {
  const path = name.split(/[?#]/, 1)[0] ?? '';
  const hashes: string[] = [];
  for (const encoded of path.split('/')) {
    if (encoded.length === 0) continue;
    let segment = encoded;
    try {
      segment = decodeURIComponent(encoded);
    } catch {
      // Malformed encoding retains the original segment.
    }
    let hash = 0x811c9dc5;
    const length = Math.min(segment.length, 128);
    for (let index = 0; index < length; index++) {
      // eslint-disable-next-line no-bitwise
      hash = Math.imul(hash ^ segment.charCodeAt(index), 0x01000193) >>> 0;
    }
    hashes.push(hash.toString(16).padStart(8, '0'));
    if (hashes.length === 12) break;
  }
  return hashes;
};

export const sameAnonymousRoute = (
  first: string[] | undefined,
  second: string[] | undefined
): boolean =>
  first === second ||
  (first !== undefined &&
    second !== undefined &&
    first.length === second.length &&
    first.every((segment, index) => segment === second[index]));
