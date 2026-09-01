require("/scripts/core/core.lua")
require("/scripts/helper_tween.lua")

sys_resume_floating_card = class(sys_resume_card)

function sys_resume_floating_card.start(arg_1_0)
	sys_resume_card.start(arg_1_0)

	if not arg_1_0.parent then
		arg_1_0.parent = arg_1_0:getParentNode()
	end

	if arg_1_0.idleAnimation then
		arg_1_0.tween = arg_1_0.idleAnimation:run(arg_1_0)
	end

	arg_1_0:refresh()
	table.insert(system.hud, arg_1_0)

	arg_1_0.is_floating = true
end

function sys_resume_floating_card.setSelected(arg_2_0)
	if arg_2_0.selectedAnimation then
		arg_2_0.selectedAnimation:run(arg_2_0)
	end

	if arg_2_0.active_component then
		arg_2_0.active_component:enable()
	end
end

function sys_resume_floating_card.setUnselected(arg_3_0)
	if arg_3_0.unselectedAnimation then
		arg_3_0.unselectedAnimation:run(arg_3_0)
	end

	if arg_3_0.active_component then
		arg_3_0.active_component:disable()
	end
end

function sys_resume_floating_card.activate(arg_4_0)
	sys_resume_card.activate(arg_4_0)
end

function sys_resume_floating_card.deactivate(arg_5_0)
	sys_resume_card.deactivate(arg_5_0)
end

function sys_resume_floating_card.setParent(arg_6_0, arg_6_1)
	local var_6_0 = arg_6_1 or arg_6_0.parent
	local var_6_1, var_6_2 = arg_6_0:getWorldPosition()

	arg_6_0:setNewParent(var_6_0)
	arg_6_0:setWorldPosition(var_6_1, var_6_2)
	tween_stop(arg_6_0.tween)

	arg_6_0.tween = Tween:moveTo(arg_6_0, 0.2, 0, 0, Ease.outExpo):start()
end

function sys_resume_floating_card.refresh(arg_7_0)
	if arg_7_0.register_button and arg_7_0.register_label then
		local var_7_0, var_7_1 = arg_7_0.register_label:getSize()
		local var_7_2 = arg_7_0.register_label:getWrapWidth()

		if var_7_2 < var_7_0 then
			arg_7_0.register_label:setTextScale(var_7_2 / var_7_0, 1)
		end

		local var_7_3, var_7_4 = arg_7_0.register_label:getTextScale()
		local var_7_5 = arg_7_0.register_label:getSize() * var_7_3
		local var_7_6
		local var_7_7, var_7_8 = arg_7_0.register_button:getSize()
		local var_7_9 = var_7_5 + var_7_7
		local var_7_10 = 2
		local var_7_11 = -6

		arg_7_0.register_button:getNode():setLocalPosition(var_7_7 / 2 - var_7_9 / 2 - var_7_10, 0)
		arg_7_0.register_label:getNode():setLocalPosition(-var_7_5 / 2 + var_7_9 / 2 + var_7_10, 5)
		arg_7_0.register_label:setLineHeightOffset(var_7_11)
	end
end
