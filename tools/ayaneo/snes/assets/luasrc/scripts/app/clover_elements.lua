require("/scripts/core/core.lua")
require("/scripts/app/clover_util.lua")

CloverElementsComponent = class()

function CloverElementsComponent.start(arg_1_0)
	CloverSingleton.Bind(CloverElementsComponent, arg_1_0)
end

function CloverElementsComponent.stop(arg_2_0)
	CloverSingleton.Reset(CloverElementsComponent)
end

local function var_0_0(arg_3_0, arg_3_1)
	return CloverSingleton.Instance(CloverElementsComponent)[arg_3_1]
end

local function var_0_1(arg_4_0, arg_4_1, arg_4_2)
	print("----- CloverElementsに直接値を設定しようとしました -----")
	assert(false)
end

CloverElements = {}

setmetatable(CloverElements, {
	__index = var_0_0,
	__newindex = var_0_1
})
