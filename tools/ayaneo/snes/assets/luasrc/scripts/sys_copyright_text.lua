require("/scripts/core/core.lua")
require("/scripts/test_program.lua")
require("/scripts/app/clover_pad.lua")

sys_copyright_text = class(gui_container)

function sys_copyright_text.start(arg_1_0)
	arg_1_0:resetText()

	arg_1_0.labelComponentTmp = createComponentFromType(COMPONENT_TYPE_LABEL)
end

function sys_copyright_text.initText(arg_2_0)
	local var_2_0 = 10
	local var_2_1 = var_2_0
	local var_2_2 = arg_2_0.labelNode:getChildByName("scrollbar")
	local var_2_3 = var_2_2:getChildByName("arrow_up")
	local var_2_4 = var_2_2:getChildByName("arrow_down")
	local var_2_5 = var_2_3:getComponent(SpriteComponent)
	local var_2_6 = var_2_4:getComponent(SpriteComponent)
	local var_2_7 = var_2_2:getChildByName("bar")
	local var_2_8 = arg_2_0.labelNode:getComponents(LabelComponent)
	local var_2_9 = var_2_8[1]
	local var_2_10 = var_2_8[2]

	var_2_8[1]:disable()
	var_2_8[2]:disable()
	var_2_2:disable()

	arg_2_0.font = arg_2_0.copyrightFont

	local var_2_11, var_2_12 = var_2_9:getTextScale()

	arg_2_0.lineHeight = arg_2_0.font:getLineHeight() + var_2_9:getLineHeightOffset()

	local var_2_13 = math.floor(var_2_2:getLocalPositionX())

	arg_2_0.labelComponentTmp:setFontResource(arg_2_0.font)
	arg_2_0.labelComponentTmp:setFlags(var_2_9:getFlags())
	arg_2_0.labelComponentTmp:setWrapWidth(var_2_13)
	arg_2_0.labelComponentTmp:setTextScale(var_2_11, var_2_12)
	arg_2_0.labelComponentTmp:setColor(var_2_9:getColor())
	arg_2_0.labelComponentTmp:setCharacterSpacingOffset(var_2_9:getCharacterSpacingOffset())
	arg_2_0.labelComponentTmp:setLineHeightOffset(var_2_9:getLineHeightOffset())

	arg_2_0.displayHeight = -var_2_4:getLocalPositionY()
	arg_2_0.displayBottomLine = arg_2_0.displayHeight / arg_2_0.lineHeight

	local var_2_14, var_2_15 = var_2_5:getSize()
	local var_2_16, var_2_17 = var_2_6:getSize()

	arg_2_0.scrollbarBodyYOffset = var_2_15 * 0.5
	arg_2_0.scrollbarBodyMaxHeight = arg_2_0.displayHeight - (var_2_15 * 0.5 + var_2_17 * 0.5)
	arg_2_0.barNode = var_2_7
	arg_2_0.barSpr = var_2_7:getComponent(SpriteComponent)
	arg_2_0.texts = {}

	function exportTextArray(arg_3_0, arg_3_1)
		for iter_3_0 = 1, #arg_3_0 do
			arg_2_0.texts[#arg_2_0.texts + 1] = {
				line = arg_3_0[iter_3_0],
				font = arg_3_1
			}
		end
	end

	function exportTextNoWordWrap(arg_4_0, arg_4_1)
		local var_4_0 = 1
		local var_4_1 = string.len(arg_4_0)
		local var_4_2 = ""
		local var_4_3 = 0

		while var_4_0 <= var_4_1 do
			local var_4_4 = ""

			while var_4_0 <= var_4_1 do
				local var_4_5 = string.sub(arg_4_0, var_4_0, var_4_0)

				var_4_0 = var_4_0 + 1

				if var_4_5 == "\n" then
					break
				end

				var_4_4 = var_4_4 .. var_4_5
			end

			if var_4_4 ~= "" then
				arg_2_0.texts[#arg_2_0.texts + 1] = {
					line = var_4_4,
					font = arg_4_1
				}
				var_4_3 = var_4_3 + 1

				if var_4_3 > 500 then
					coroutine.yield(false)

					var_4_3 = 0
				end
			end
		end
	end

	function exportText(arg_5_0, arg_5_1)
		local var_5_0 = arg_2_0.lineHeight * 2 - 1
		local var_5_1 = ""
		local var_5_2 = {}
		local var_5_3 = 1
		local var_5_4 = 0
		local var_5_5 = 0
		local var_5_6 = 1
		local var_5_7 = string.len(arg_5_0)
		local var_5_8 = 128
		local var_5_9 = 191
		local var_5_10 = 127
		local var_5_11 = 194
		local var_5_12 = 223
		local var_5_13 = 239
		local var_5_14 = 247
		local var_5_15 = 251
		local var_5_16 = 253

		while var_5_6 <= var_5_7 do
			var_5_2[var_5_3] = {
				width = var_5_4,
				index = var_5_6
			}

			local var_5_17 = ""
			local var_5_18 = 0

			repeat
				local var_5_19 = string.sub(arg_5_0, var_5_6, var_5_6)
				local var_5_20 = string.byte(var_5_19)

				if var_5_20 <= var_5_10 then
					var_5_17 = var_5_17 .. var_5_19
					var_5_6 = var_5_6 + 1

					break
				end

				if var_5_20 < var_5_11 or var_5_16 < var_5_20 then
					var_5_6 = var_5_6 + 1
					var_5_18 = 1

					break
				end

				local var_5_21 = string.sub(arg_5_0, var_5_6 + 1, var_5_6 + 1)
				local var_5_22 = string.byte(var_5_21)

				if not (var_5_8 <= var_5_22 and var_5_22 <= var_5_9) then
					var_5_6 = var_5_6 + 1
					var_5_18 = 1

					break
				end

				if var_5_20 <= var_5_12 then
					var_5_17 = var_5_17 .. var_5_19 .. var_5_21
					var_5_6 = var_5_6 + 2

					break
				end

				local var_5_23 = string.sub(arg_5_0, var_5_6 + 2, var_5_6 + 2)
				local var_5_24 = string.byte(var_5_23)

				if not (var_5_8 <= var_5_24 and var_5_24 <= var_5_9) then
					var_5_6 = var_5_6 + 1
					var_5_18 = 1

					break
				end

				if var_5_20 <= var_5_13 then
					var_5_17 = var_5_17 .. var_5_19 .. var_5_21 .. var_5_23
					var_5_6 = var_5_6 + 3

					break
				end

				local var_5_25 = string.sub(arg_5_0, var_5_6 + 3, var_5_6 + 3)
				local var_5_26 = string.byte(var_5_25)

				if not (var_5_8 <= var_5_26 and var_5_26 <= var_5_9) then
					var_5_6 = var_5_6 + 1
					var_5_18 = 1

					break
				end

				if var_5_20 <= var_5_14 then
					var_5_17 = var_5_17 .. var_5_19 .. var_5_21 .. var_5_23 .. var_5_25
					var_5_6 = var_5_6 + 4

					break
				end

				local var_5_27 = string.sub(arg_5_0, var_5_6 + 4, var_5_6 + 4)
				local var_5_28 = string.byte(var_5_27)

				if not (var_5_8 <= var_5_28 and var_5_28 <= var_5_9) then
					var_5_6 = var_5_6 + 1
					var_5_18 = 1

					break
				end

				if var_5_20 <= var_5_15 then
					var_5_17 = var_5_17 .. var_5_19 .. var_5_21 .. var_5_23 .. var_5_25 .. var_5_27
					var_5_6 = var_5_6 + 5

					break
				end

				local var_5_29 = string.sub(arg_5_0, var_5_6 + 5, var_5_6 + 5)
				local var_5_30 = string.byte(var_5_29)

				if not (var_5_8 <= var_5_30 and var_5_30 <= var_5_9) then
					var_5_6 = var_5_6 + 1
					var_5_18 = 1

					break
				end

				var_5_17 = var_5_17 .. var_5_19 .. var_5_21 .. var_5_23 .. var_5_25 .. var_5_27 .. var_5_29
				var_5_6 = var_5_6 + 6

				break
			until true

			if var_5_18 > 0 then
				print("Error: found Unknown utf8 char code")
			end

			var_5_2[var_5_3].indexNext = var_5_6

			local var_5_31 = var_5_1 .. var_5_17

			arg_2_0.labelComponentTmp:setText(var_5_31)

			local var_5_32

			var_5_4, var_5_32 = arg_2_0.labelComponentTmp:getSize()

			local var_5_33 = false

			if var_5_32 <= var_5_0 and var_5_4 > var_2_13 then
				var_5_32 = var_5_0 + 1
				var_5_33 = true
			end

			if var_5_0 < var_5_32 then
				if var_5_17 ~= "\n" then
					if var_5_33 then
						var_5_6 = var_5_2[var_5_3].index
					else
						for iter_5_0 = 0, var_5_3 - 1 do
							if var_5_2[var_5_3 - iter_5_0].width == var_5_4 then
								var_5_1 = string.sub(arg_5_0, var_5_2[1].index, var_5_2[var_5_3 - iter_5_0 - 1].index)
								var_5_6 = var_5_2[var_5_3 - iter_5_0 - 1].indexNext

								break
							end
						end
					end
				end

				arg_2_0.texts[#arg_2_0.texts + 1] = {
					line = var_5_1,
					font = arg_5_1
				}
				var_5_1 = ""
				var_5_3 = 1
				var_5_2[var_5_3] = {
					0,
					var_5_31
				}
				var_5_4 = 0

				if var_2_1 then
					var_2_1 = var_2_1 - 1
				end

				if var_2_1 <= 0 then
					coroutine.yield(false)

					var_2_1 = var_2_0
				end
			else
				var_5_1 = var_5_31
				var_5_3 = var_5_3 + 1
			end
		end

		local var_5_34

		arg_2_0.texts[#arg_2_0.texts + 1] = {
			line = var_5_1,
			font = arg_5_1
		}
	end

	if arg_2_0.textArrayLua then
		-- OSS lines supplied as a Lua table (see sys_copyright.start) - read directly, no host bridge.
		exportTextArray(arg_2_0.textArrayLua, arg_2_0.headerTextFont)
	elseif var_2_9:getTextArray() then
		exportTextArray(var_2_9:getTextArray(), arg_2_0.headerTextFont)
	elseif var_2_9:hasWordWrap() then
		exportText(var_2_9:getText(), arg_2_0.headerTextFont)
	else
		exportTextNoWordWrap(var_2_9:getText(), arg_2_0.headerTextFont)
	end

	coroutine.yield(false)

	if arg_2_0.additionalText then
		if var_2_9:hasWordWrap() then
			exportText(arg_2_0.additionalText, arg_2_0.additionalTextFont)
		else
			exportTextNoWordWrap(arg_2_0.additionalText, arg_2_0.additionalTextFont)
		end

		coroutine.yield(false)
	end

	if var_2_9:hasWordWrap() then
		exportText(var_2_10:getText())
	else
		exportTextNoWordWrap(var_2_10:getText())
	end

	if arg_2_0.labelObjs == nil then
		arg_2_0.labelObjs = {}

		local var_2_18 = 2 + math.floor(arg_2_0.displayBottomLine)

		for iter_2_0 = 1, var_2_18 do
			local var_2_19 = {
				component = arg_2_0:createLabelComponent(arg_2_0.labelComponentTmp)
			}

			arg_2_0.linesNode:attachComponent(var_2_19.component)

			arg_2_0.labelObjs[iter_2_0] = var_2_19

			local var_2_20 = -(iter_2_0 - 1) * (arg_2_0.font:getLineHeight() / var_2_12 + var_2_9:getLineHeightOffset()) - var_2_9:getLineHeightOffset()

			var_2_19.component:setTextOffset(0, var_2_20)
		end

		coroutine.yield(false)
	end

	for iter_2_1, iter_2_2 in pairs(arg_2_0.labelObjs) do
		iter_2_2.component:enable()
	end

	coroutine.yield(false)

	if false then
		print("=========================")

		for iter_2_3, iter_2_4 in pairs(arg_2_0.texts) do
			print(string.format("%3d %s", iter_2_3, iter_2_4.line))
		end

		print("=========================")
	end

	arg_2_0.userControl = true
	arg_2_0.scrollLine = -#arg_2_0.labelObjs
	arg_2_0.scrollLineRequest = arg_2_0.scrollLine
	arg_2_0.scrollLineCurMin = 0
	arg_2_0.scrollLineCurMax = #arg_2_0.texts - #arg_2_0.labelObjs + 2
	arg_2_0.scrollLineMin = 0
	arg_2_0.scrollLineMax = #arg_2_0.texts - #arg_2_0.labelObjs + 2

	if arg_2_0.scrollLineMax < arg_2_0.scrollLineMin then
		arg_2_0.scrollLineMax = arg_2_0.scrollLineMin
	end

	arg_2_0.stopScroll = true

	var_2_2:enable()

	return true
end

function sys_copyright_text.update(arg_6_0, arg_6_1)
	if not arg_6_0.initialized then
		arg_6_0.initialized = arg_6_0:initTextCo()

		if not arg_6_0.initialized then
			return
		end
	end

	if not arg_6_0.labelNode:isEnabled() then
		arg_6_0.labelNode:enable()
	end

	if arg_6_0.resetScrollRequest then
		arg_6_0.userControl = true
		arg_6_0.stopScroll = false
		arg_6_0.scrollLineCurMin = 0
		arg_6_0.scrollLineCurMax = #arg_6_0.texts - #arg_6_0.labelObjs + 2
		arg_6_0.scrollLine = arg_6_0.scrollLineCurMin
		arg_6_0.scrollLineRequest = arg_6_0.scrollLine
		arg_6_0.resetScrollRequest = false
	end

	local var_6_0 = 0

	if not arg_6_0.stopScroll then
		if not arg_6_0.userControl then
			var_6_0 = 1
		end

		local var_6_1 = #arg_6_0.labelObjs - 1

		if CloverPadUI.pressed.up then
			arg_6_0.userControl = true

			if arg_6_0.scrollLineRequest > arg_6_0.scrollLineMin then
				arg_6_0.scrollLineRequest = math.floor(arg_6_0.scrollLineRequest) - var_6_1
			end

			if arg_6_0.scrollLineRequest < arg_6_0.scrollLineMin then
				arg_6_0.scrollLineRequest = arg_6_0.scrollLineMin
			end
		end

		if CloverPadUI.pressed.down then
			arg_6_0.userControl = true

			if arg_6_0.scrollLineRequest < arg_6_0.scrollLineMax then
				arg_6_0.scrollLineRequest = math.floor(arg_6_0.scrollLineRequest) + var_6_1
			end

			if arg_6_0.scrollLineRequest > arg_6_0.scrollLineMax then
				arg_6_0.scrollLineRequest = arg_6_0.scrollLineMax
			end
		end

		if not arg_6_0.userControl then
			arg_6_0.scrollLine = arg_6_0.scrollLine + arg_6_1 * var_6_0
			arg_6_0.scrollLineRequest = arg_6_0.scrollLine
		else
			local var_6_2 = arg_6_0.scrollLineRequest - arg_6_0.scrollLine
			local var_6_3 = 1

			if var_6_2 < 0 then
				arg_6_0.scrollLine = arg_6_0.scrollLine + (var_6_2 - 0.1) * var_6_3

				if arg_6_0.scrollLine < arg_6_0.scrollLineRequest then
					arg_6_0.scrollLine = arg_6_0.scrollLineRequest
				end
			elseif var_6_2 > 0 then
				arg_6_0.scrollLine = arg_6_0.scrollLine + (var_6_2 + 0.1) * var_6_3

				if arg_6_0.scrollLine > arg_6_0.scrollLineRequest then
					arg_6_0.scrollLine = arg_6_0.scrollLineRequest
				end
			end
		end
	end

	if arg_6_0.scrollLine < arg_6_0.scrollLineCurMin then
		arg_6_0.scrollLine = arg_6_0.scrollLineCurMin
	elseif arg_6_0.scrollLine > arg_6_0.scrollLineCurMax then
		arg_6_0.scrollLine = arg_6_0.scrollLineCurMax
	end

	local var_6_4 = arg_6_0.scrollLine

	if var_6_4 < 0 then
		var_6_4 = 0
	end

	local var_6_5 = arg_6_0.scrollLine + (#arg_6_0.labelObjs - 1)

	if var_6_5 > #arg_6_0.texts + 1 then
		var_6_5 = #arg_6_0.texts + 1
	end

	local var_6_6 = #arg_6_0.texts - (#arg_6_0.labelObjs - 1)

	if var_6_6 < 1 then
		var_6_6 = 1
	end

	local var_6_7
	local var_6_8
	local var_6_9 = arg_6_0.scrollLine / var_6_6
	local var_6_10 = 1

	if var_6_9 <= 0 then
		var_6_9 = 0

		local var_6_11 = 0.5

		var_6_7 = true
	elseif var_6_9 > 1 then
		var_6_9 = 1

		local var_6_12 = 0.5

		var_6_8 = true
	end

	if arg_6_0.arrowUpDisable ~= var_6_7 then
		arg_6_0.arrowUpDisable = var_6_7

		if var_6_7 then
			arg_6_0.arrowUpOff:enable()
			arg_6_0.arrowUpOn:disable()
		else
			arg_6_0.arrowUpOff:disable()
			arg_6_0.arrowUpOn:enable()
		end
	end

	if arg_6_0.arrowDownDisable ~= var_6_8 then
		arg_6_0.arrowDownDisable = var_6_8

		if var_6_8 then
			arg_6_0.arrowDownOff:enable()
			arg_6_0.arrowDownOn:disable()
		else
			arg_6_0.arrowDownOff:disable()
			arg_6_0.arrowDownOn:enable()
		end
	end

	local var_6_13 = -var_6_9 * arg_6_0.scrollbarBodyMaxHeight - arg_6_0.scrollbarBodyYOffset
	local var_6_14 = var_6_5 - var_6_4

	if var_6_14 < 0 then
		var_6_14 = 0
	end

	local var_6_15 = var_6_14 / #arg_6_0.texts

	if var_6_15 > 1 then
		var_6_15 = 1
	end

	local var_6_16 = var_6_15 * arg_6_0.scrollbarBodyMaxHeight
	local var_6_17, var_6_18 = arg_6_0.barSpr:getSize()
	local var_6_19 = 0
	local var_6_20 = 32

	if var_6_16 < var_6_20 then
		var_6_19 = var_6_20 - var_6_16
	end

	local var_6_21 = var_6_9 + var_6_15

	if var_6_21 > 1 then
		var_6_21 = 1
	end

	local var_6_22 = -0.5 + var_6_9 * var_6_21
	local var_6_23 = -0.5 + var_6_9

	arg_6_0.barNode:setLocalPositionY(var_6_13 + var_6_16 * var_6_22 + var_6_19 * var_6_23)
	arg_6_0.barSpr:setSize(var_6_17, var_6_16 + var_6_19)

	local var_6_24 = arg_6_0.scrollLine - math.floor(arg_6_0.scrollLine)
	local var_6_25 = 1 + math.floor(arg_6_0.scrollLine)
	local var_6_26 = arg_6_0.headerTextFont or arg_6_0.additionalTextFont
	local var_6_27 = var_6_24 * arg_6_0.lineHeight

	arg_6_0.linesNode:setLocalPositionY(var_6_27)

	for iter_6_0, iter_6_1 in pairs(arg_6_0.labelObjs) do
		local var_6_28 = arg_6_0.texts[var_6_25 + iter_6_0 - 1]
		local var_6_29
		local var_6_30

		if var_6_28 then
			var_6_29 = var_6_28.line
			var_6_30 = var_6_28.font
		else
			var_6_29 = nil
			var_6_30 = nil
		end

		if var_6_29 then
			iter_6_1.component:setText(var_6_29)

			if var_6_26 then
				if var_6_30 then
					iter_6_1.component:setFontResource(var_6_30)
				else
					iter_6_1.component:setFontResource(arg_6_0.font)
				end
			end
		else
			iter_6_1.component:setText("")
		end
	end

	if var_6_8 then
		test_program:update(arg_6_1)
	end
end

function sys_copyright_text.createLabelComponent(arg_7_0, arg_7_1)
	local var_7_0 = createComponentFromType(COMPONENT_TYPE_LABEL)

	var_7_0:setText("")
	var_7_0:setFontResource(arg_7_1:getFontResource())
	var_7_0:setFlags(arg_7_1:getFlags())
	var_7_0:setWrapWidth(InfiniteWrapWidth)
	var_7_0:setTextScale(arg_7_1:getTextScale())
	var_7_0:setCharacterSpacingOffset(arg_7_1:getCharacterSpacingOffset())
	var_7_0:setLineHeightOffset(arg_7_1:getLineHeightOffset())

	local var_7_1, var_7_2, var_7_3, var_7_4 = arg_7_1:getColor()

	var_7_0:setColor(var_7_1, var_7_2, var_7_3, var_7_4)

	return var_7_0
end

function sys_copyright_text.resetScroll(arg_8_0)
	arg_8_0.resetScrollRequest = true

	arg_8_0.labelNode:disable()
end

function sys_copyright_text.resetText(arg_9_0)
	arg_9_0.initialized = false
	arg_9_0.initTextCo = coroutine.wrap(sys_copyright_text.initText)
	arg_9_0.labelNode = arg_9_0:getChildByName("text")

	arg_9_0.labelNode:getChildByName("scrollbar"):disable()

	local var_9_0 = arg_9_0.labelNode:getComponents(LabelComponent)
	local var_9_1 = var_9_0[1]
	local var_9_2 = var_9_0[2]

	var_9_0[1]:disable()
	var_9_0[2]:disable()

	if arg_9_0.labelObjs then
		for iter_9_0, iter_9_1 in pairs(arg_9_0.labelObjs) do
			iter_9_1.component:disable()
		end
	end
end

function sys_copyright_text.setHeaderTextFont(arg_10_0, arg_10_1)
	arg_10_0.headerTextFont = arg_10_1
end

function sys_copyright_text.setAdditionalTextFont(arg_11_0, arg_11_1)
	arg_11_0.additionalTextFont = arg_11_1
end

function sys_copyright_text.addAdditionalText(arg_12_0, arg_12_1)
	arg_12_0.additionalText = arg_12_1
end
