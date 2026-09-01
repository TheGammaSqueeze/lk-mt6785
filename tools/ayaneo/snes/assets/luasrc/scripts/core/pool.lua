require("core.lua")

local var_0_0 = pairs
local var_0_1 = setmetatable

Pool = class()

function Pool.alloc(arg_1_0)
	local var_1_0 = arg_1_0[0]

	if var_1_0 == nil then
		local var_1_1 = arg_1_0.mt

		if var_1_1 then
			return (var_0_1({}, var_1_1))
		else
			return {}
		end
	elseif var_1_0 == 1 then
		local var_1_2 = arg_1_0[1]

		arg_1_0[1] = nil
		arg_1_0[0] = nil

		return var_1_2
	else
		local var_1_3 = arg_1_0[var_1_0]

		arg_1_0[var_1_0] = nil
		arg_1_0[0] = var_1_0 - 1

		return var_1_3
	end
end

function Pool.free(arg_2_0, arg_2_1)
	if not arg_2_0.keepContents then
		for iter_2_0 = #arg_2_1, 1, -1 do
			arg_2_1[iter_2_0] = nil
		end

		for iter_2_1 in var_0_0(arg_2_1) do
			arg_2_1[iter_2_1] = nil
		end
	end

	local var_2_0 = arg_2_0[0]

	if var_2_0 == nil then
		arg_2_0[0] = 1
		arg_2_0[1] = arg_2_1
	else
		arg_2_0[0] = var_2_0 + 1
		arg_2_0[var_2_0 + 1] = arg_2_1
	end
end

function createPooledNewAndFreeMethods(arg_3_0, arg_3_1)
	local var_3_0 = new(Pool)

	var_3_0.mt = arg_3_0
	var_3_0.keepContents = arg_3_1

	function arg_3_0.new(...)
		local var_4_0 = var_3_0:alloc()

		if var_4_0.init then
			var_4_0:init(...)
		end

		return var_4_0
	end

	function arg_3_0.free(arg_5_0)
		if arg_5_0.destroy then
			arg_5_0:destroy()
		end

		var_3_0:free(arg_5_0)
	end
end
