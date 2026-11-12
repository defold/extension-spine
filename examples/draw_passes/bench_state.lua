-- How the draw_passes render script draws the benchmark models. Shared between the render script and
-- draw_passes.script (game.project sets script.shared_state = 1).
local M = {
	OFF = 0,      -- Benchmark models are not drawn (any other example)
	VIEWS = 1,    -- view_count render.draw() calls, each drawing all models into its own side-by-side view
	DISJOINT = 2, -- Three render.draw() calls, each drawing a different third of the models
}
M.mode = M.OFF
M.view_count = 1
return M
