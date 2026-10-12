# Enlarged book images

Implements [#28](https://github.com/RigleGit/3dslibris/issues/28) for images
already displayed in EPUB, MOBI and FB2. It does not add image rendering to
other text formats or change PDF/CBZ/XPS navigation.

Hold `Y` to select links and images. Use the existing directional controls to
move through targets, including on the other screen and adjacent pages. `A`
follows a text link or opens an image. A linked image has two actions: `A`
opens the image and `X` follows its link. The status bar shows these actions.
`B` or `Y` ends selection. A short `Y` still toggles a bookmark.

Normal touch links keep their existing action. During selection, touching an
image on the touch screen selects it; press `A` to open it.

In the image viewer:

- `A` / `B`: zoom in / out, from fit to 4× fit, subject to memory limits.
- Circle Pad or D-Pad: pan in the reading orientation.
- Stylus: move the visible region using the overview on the touch screen.
- `X`: fit and center.
- `Y` or touch `back (Y)`: return to the same page and selection.
- `START`: return to the library.
- `SELECT`: close the viewer and open the book settings.

The image starts fitted and centered. When movement stops, including pad
movement, a filtered frame replaces the fast movement frame. The viewer never
changes the book's page, bookmarks, preferences or stored search query.

GIF images show their first frame. Very large or unsupported images may be
omitted or report a loading error when they exceed memory limits.
