import type { Lom, Root } from '../models/lom';
import type { Offset, Rect } from '../types/geometry';

/**
 * Walks `tree` depth-first, returning the root with the lowest depth index.
 */
export const findTargetRoot = (
  lom: Lom | undefined,
  position: Offset
): Root | null => {
  if (!lom || !lom.root) return null;

  let deepestMatch: Root | null = null;

  const traverse = (root: Root) => {
    if (isInside(position, root.box)) {
      if (!deepestMatch || root.depth >= deepestMatch.depth) {
        deepestMatch = root;
      }

      if (root.children && root.children.length > 0) {
        for (let i = 0; i < root.children.length; i++) {
          const child = root.children[i];
          if (child) traverse(child);
        }
      }
    }
  };

  traverse(lom.root);

  return deepestMatch;
};

/**
 * Function to know if the `position` is inside into `box`
 */
const isInside = (position: Offset, box: Rect): boolean => {
  let x = (box as any).x ?? box.left ?? 0;
  let y = (box as any).y ?? box.top ?? 0;
  let width = box.width;
  let height = box.height;

  if (width === 0 || height === 0) return false;

  return (
    position.x >= x &&
    position.x <= x + width &&
    position.y >= y &&
    position.y <= y + height
  );
};
