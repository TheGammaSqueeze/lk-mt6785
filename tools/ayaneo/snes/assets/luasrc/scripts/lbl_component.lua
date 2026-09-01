require("/scripts/core/core.lua")
require("/scripts/env/env_common.lua")

lbl_component = class(LabelComponent)

function lbl_component.start(arg_1_0)
	local var_1_0 = Env.colors[arg_1_0.colorName]

	if var_1_0 then
		arg_1_0:setColor(var_1_0[1], var_1_0[2], var_1_0[3], var_1_0[4])
	elseif arg_1_0:getNode() then
		arg_1_0:setColor(1, 0, 1, 1)
	end

	if arg_1_0.fontName and sys_boot.instance.fonts[arg_1_0.fontName] then
		arg_1_0:setFontResource(sys_boot.instance.fonts[arg_1_0.fontName])
	end

	if arg_1_0.lineHeightOffset then
		arg_1_0:setLineHeightOffset(arg_1_0.lineHeightOffset)
	end

	if arg_1_0.characterSpacingOffset then
		arg_1_0:setCharacterSpacingOffset(arg_1_0.characterSpacingOffset)
	end

	if arg_1_0.textScale then
		arg_1_0:setTextScale(arg_1_0.textScale, arg_1_0.textScale)
	end
end

function lbl_component.setTextArray(arg_2_0, arg_2_1)
	arg_2_0.textArray = arg_2_1
end

function lbl_component.getTextArray(arg_3_0)
	return arg_3_0.textArray
end
