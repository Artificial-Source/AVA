// Closed, invented browser-preview asset. Paths and tool output never select media.
export type ToolImagePreviewId = 'search-results-example';

const searchResultsImage = Object.freeze({
  src: '/tool-previews/search-results.png',
  alt: 'Supplied example: search results with a search field, Recent filter, and three result rows.',
  label: 'Supplied image · Search results example',
  width: 640,
  height: 400,
} as const);

export function isToolImagePreviewId(value: unknown): value is ToolImagePreviewId {
  return value === 'search-results-example';
}

export function toolImagePreview(value: unknown) {
  // Scalar equality deliberately avoids property lookup, coercion and URL construction.
  return isToolImagePreviewId(value) ? searchResultsImage : null;
}
