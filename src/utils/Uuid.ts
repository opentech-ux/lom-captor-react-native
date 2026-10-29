import uuid from 'react-native-uuid';

/**
 * Creates an RFC 9562 UUID v7 without adding another randomness dependency.
 * `react-native-uuid` supplies entropy through its RN-compatible v4 generator;
 * this function applies the v7 timestamp, version, and variant layout.
 */
export const createUuidV7 = (): string => {
  const randomUuid = uuid.v4();

  if (typeof randomUuid !== 'string') {
    throw new Error('react-native-uuid did not return a string UUID');
  }

  const randomHex = randomUuid.replaceAll('-', '').toLowerCase();
  if (!/^[0-9a-f]{32}$/.test(randomHex)) {
    throw new Error('react-native-uuid returned an invalid UUID');
  }

  const timestampHex = Date.now().toString(16).padStart(12, '0');
  const variant = (8 + (Number.parseInt(randomHex.charAt(3), 16) % 4)).toString(
    16
  );
  const randB =
    randomHex.slice(4, 12) + randomHex.slice(13, 16) + randomHex.slice(17, 21);

  return [
    timestampHex.slice(0, 8),
    timestampHex.slice(8, 12),
    `7${randomHex.slice(0, 3)}`,
    `${variant}${randB.slice(0, 3)}`,
    randB.slice(3),
  ].join('-');
};
