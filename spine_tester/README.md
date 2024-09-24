# Simple Spine Animations Tester

## Installation

1. Add IMGUI into your dependencies in `game.project` https://github.com/britzl/extension-imgui/ (e.g. https://github.com/britzl/extension-imgui/archive/refs/heads/master.zip)

2. `Project`->`Fetch Libraries`

3. Copy `spine_tester` folder into your project folder.

4. In `game.project` replace `Bootstrap`->`Main Collection` with `/spine_tester/tester.collection`

5. In `game.project` replace `Input`->`Game Binding` with `/imgui/bindings/imgui.input_binding`

6. Open terminal in your project folder and run:

>python ./spine_tester/create_tester.py

7. Run the project and use the interface to play any animations you want, or all of them one by one.
It's also possible to zoom in/zoom out using scroll or move animations by clicking anywhere on the screen and moving the mouse.

"Play All" runs all the animations on all models, starting with the currently selected model and animation, until the last animation on the last model.
