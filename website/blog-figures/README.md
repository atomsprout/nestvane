# Figures for the blog

The drawings in the blog posts are these HTML pages, rendered to PNG with Chrome and saved next to the
post that uses them. To change the words on a figure, edit its page and render it again:

```sh
"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" --headless=new --hide-scrollbars \
  --force-device-scale-factor=1.5 --window-size=1200,860 \
  --screenshot=../blog/2026-11-22-android-17-inside-an-iphone-app/timeline.png file://$PWD/timeline.html
```

The window height is the figure's height: 860 for the timeline, 610 for the boot chart, 790 for the
architecture, 700 for the registers and 760 for the two sheets of screenshots.
