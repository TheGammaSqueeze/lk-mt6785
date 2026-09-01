require("/scripts/core/core.lua")

effect_smoke = class(WorldNode)

function effect_smoke.start(arg_1_0)
	arg_1_0.L:disable()
	arg_1_0.S:disable()
end

function effect_smoke.run(arg_2_0)
	arg_2_0:runL()
end

function effect_smoke.runL(arg_3_0)
	arg_3_0.L:enable()
	arg_3_0.L:restart()
end

function effect_smoke.runM(arg_4_0)
	arg_4_0:runL()
end

function effect_smoke.runS(arg_5_0)
	arg_5_0.S:enable()
	arg_5_0.S:restart()
end
