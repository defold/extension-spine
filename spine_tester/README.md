# Simple Spine Animations Tester

## Installation

1. Copy `spine_tester` folder into your project folder.

2. Open terminal in your project folder and run:

>python ./spine_tester/create_tester.py

It's also possible to remove all the `*.go`, `*.gui`, `*.collection` and `hooks.editor_script` files if it runs with `cleanup` argument:

>python ./spine_tester/create_tester.py cleanup

It maybe useful in cases you want to run the editor faster on a big project.

3. If project is already opened click: `Project`->`Fetch Libraries` or open project

4. Run the project and use the interface to play any animations you want, or all of them one by one.
It's also possible to zoom in/zoom out using scroll or move animations by clicking anywhere on the screen and moving the mouse.

"Play All" runs all the animations on all models, starting with the currently selected model and animation, until the last animation on the last model.
