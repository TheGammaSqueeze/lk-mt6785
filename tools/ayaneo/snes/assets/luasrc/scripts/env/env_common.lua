require("/scripts/core/core.lua")

Env = {}

local var_0_0 = {}

local function var_0_1(arg_1_0, arg_1_1)
	if var_0_0[arg_1_1] == nil then
		Env.init()
	end

	return var_0_0[arg_1_1]
end

local function var_0_2(arg_2_0, arg_2_1, arg_2_2)
	var_0_0[arg_2_1] = arg_2_2
end

setmetatable(Env, {
	__index = var_0_1,
	__newindex = var_0_2
})
