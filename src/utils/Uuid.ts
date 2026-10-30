import resolveSessionRecorderReactNative from '../NativeSessionRecorderReactNative';

/**
 * Creates an RFC 9562 UUID v7 using the platform CSPRNG through the private
 * TurboModule. No JS crypto global, polyfill, or Math.random fallback is used.
 * The layout retains 74 random bits alongside the timestamp/version/variant.
 */
export const createUuidV7 = (): string => {
  const randomHex = resolveSessionRecorderReactNative()?.getUuidRandomHex();

  if (typeof randomHex !== 'string' || !/^[0-9a-f]{32}$/.test(randomHex)) {
    throw new Error('Native UUID entropy is unavailable or invalid');
  }

  const timestamp = Date.now();
  if (
    !Number.isSafeInteger(timestamp) ||
    timestamp < 0 ||
    timestamp > 0xffffffffffff
  ) {
    throw new Error('UUID v7 timestamp is outside the unsigned 48-bit range');
  }
  const timestampHex = timestamp.toString(16).padStart(12, '0');
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
