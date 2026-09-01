require("gui_layout.lua")

local var_0_0 = math.huge

GUIHLayout = class(GUILayout)

function GUIHLayout.refresh(arg_1_0)
	local var_1_0 = var_0_0
	local var_1_1 = -var_0_0
	local var_1_2 = var_0_0
	local var_1_3 = -var_0_0
	local var_1_4 = arg_1_0.nodesIndexes
	local var_1_5 = #var_1_4
	local var_1_6 = 0
	local var_1_7 = 0
	local var_1_8 = arg_1_0.columnsCount

	arg_1_0.rowInfos = {}
	arg_1_0.columnInfos = {}

	local var_1_9 = 1
	local var_1_10 = 1
	local var_1_11 = {
		posY = 0,
		height = 0
	}

	arg_1_0.rowInfos[var_1_9] = var_1_11

	for iter_1_0 = 1, var_1_5 do
		local var_1_12 = var_1_4[iter_1_0]
		local var_1_13 = var_1_12.width * var_1_12.centerX
		local var_1_14 = var_1_12.height * var_1_12.centerY

		if iter_1_0 == 1 then
			var_1_6 = -var_1_13
			var_1_7 = var_1_14
		end

		local var_1_15 = {
			width = var_1_12.width,
			posX = var_1_6 + var_1_13
		}

		arg_1_0.columnInfos[var_1_10] = var_1_15

		local var_1_16 = var_1_15.posX
		local var_1_17 = var_1_11.posY

		var_1_12.posX = var_1_16
		var_1_12.posY = var_1_17

		var_1_12.node:setLocalPosition(var_1_16, var_1_17)

		local var_1_18 = var_1_6
		local var_1_19 = var_1_7
		local var_1_20 = var_1_18 + var_1_12.width
		local var_1_21 = var_1_19 - var_1_12.height

		if var_1_18 < var_1_0 then
			var_1_0 = var_1_18
		end

		if var_1_1 < var_1_20 then
			var_1_1 = var_1_20
		end

		if var_1_21 < var_1_2 then
			var_1_2 = var_1_21
		end

		if var_1_3 < var_1_19 then
			var_1_3 = var_1_19
		end

		if var_1_12.width > var_1_15.width then
			var_1_15.width = var_1_12.width
		end

		var_1_10 = var_1_10 + 1
		var_1_6 = var_1_6 + var_1_15.width + arg_1_0.horizontalGap
	end

	arg_1_0.xMin = var_1_0
	arg_1_0.xMax = var_1_1
	arg_1_0.yMin = var_1_2
	arg_1_0.yMax = var_1_3
end
