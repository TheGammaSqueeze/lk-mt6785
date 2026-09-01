require("/scripts/core/core.lua")
require("/scripts/app/clover_task.lua")
require("/scripts/app/clover_pad.lua")
require("/scripts/app/clover_const.lua")

local var_0_0 = 0.4

CloverTransition = class()

function CloverTransition.Init(arg_1_0, arg_1_1)
	CloverTask.Register("transition", CloverTransition.Update, arg_1_0)
	arg_1_0:initHideNodes(arg_1_1)
end

function CloverTransition.Update(arg_2_0, arg_2_1)
	if arg_2_0.is_running then
		arg_2_0:updateTransition(arg_2_1)
	end
end

function CloverTransition.StartTransition(arg_3_0, arg_3_1, arg_3_2, arg_3_3, arg_3_4)
	if not arg_3_0.is_running then
		if type(arg_3_1) ~= "function" then
			function arg_3_0.callback_middle()
				return
			end
		else
			arg_3_0.callback_middle = arg_3_1
		end

		if type(arg_3_2) ~= "function" then
			function arg_3_0.callback_final()
				return
			end
		else
			arg_3_0.callback_final = arg_3_2
		end

		arg_3_0.mode = arg_3_3

		if arg_3_4 then
			arg_3_0.seq = 1
			arg_3_0.alpha = 1

			arg_3_0:updateNodeAlpha()

			arg_3_0.seq = 2

			arg_3_0.callback_middle()

			arg_3_0.seq = 3
			arg_3_0.alpha = 0

			arg_3_0:updateNodeAlpha()

			arg_3_0.seq = 4
			arg_3_0.alpha = nil

			arg_3_0:updateNodeAlpha()
			arg_3_0.callback_final()

			arg_3_0.seq = nil
		else
			arg_3_0.is_running = true
		end
	end
end

function CloverTransition.setRelative(arg_6_0)
	for iter_6_0, iter_6_1 in pairs(arg_6_0.nodeInfos) do
		local var_6_0 = iter_6_1.node

		if var_6_0 then
			iter_6_1.startY = var_6_0:getLocalPositionY()
		end
	end
end

function CloverTransition.clearRelative(arg_7_0)
	for iter_7_0, iter_7_1 in pairs(arg_7_0.nodeInfos) do
		iter_7_1.startY = nil
	end
end

function CloverTransition.initHideNodes(arg_8_0, arg_8_1)
	if not arg_8_0.hideNodes then
		local var_8_0 = arg_8_1:getChildByName("homemenu")

		arg_8_0.nodeInfos = {
			cardlist = {
				y = 600,
				node = var_8_0:getChildByName("elements"):getChildByName("cardlist")
			},
			gametitle = {
				y = 600,
				node = var_8_0:getChildByName("gametitle")
			},
			hud = {
				y = -600,
				node = var_8_0:getChildByName("hud")
			},
			resume_floating = {
				y = -600,
				node = var_8_0:getChildByName("resume_floating")
			},
			thumbnails = {
				y = -600,
				node = var_8_0:getChildByName("thumbnails")
			}
		}
		arg_8_0.hideNodes = {
			DisksystemIn = {
				{
					hide = true,
					info = arg_8_0.nodeInfos.cardlist
				},
				{
					hide = true,
					info = arg_8_0.nodeInfos.gametitle
				},
				{
					hide = true,
					info = arg_8_0.nodeInfos.hud
				},
				{
					hide = true,
					info = arg_8_0.nodeInfos.resume_floating
				},
				{
					hide = true,
					info = arg_8_0.nodeInfos.thumbnails
				}
			},
			DisksystemOut = {
				{
					show = true,
					info = arg_8_0.nodeInfos.cardlist
				},
				{
					show = true,
					info = arg_8_0.nodeInfos.gametitle
				},
				{
					show = true,
					info = arg_8_0.nodeInfos.hud
				},
				{
					show = true,
					info = arg_8_0.nodeInfos.resume_floating
				},
				{
					show = true,
					info = arg_8_0.nodeInfos.thumbnails
				}
			},
			AutoPlayIn = {
				{
					show = true,
					info = arg_8_0.nodeInfos.cardlist
				},
				{
					show = true,
					info = arg_8_0.nodeInfos.gametitle
				}
			},
			AutoPlayOptionMenuIn = {
				{
					hide = true,
					info = arg_8_0.nodeInfos.hud
				},
				{
					hide = true,
					info = arg_8_0.nodeInfos.resume_floating
				},
				{
					hide = true,
					info = arg_8_0.nodeInfos.thumbnails
				},
				{
					hide = true,
					info = arg_8_0.nodeInfos.cardlist
				},
				{
					hide = true,
					info = arg_8_0.nodeInfos.gametitle
				},
				{
					show = true,
					info = arg_8_0.nodeInfos.cardlist
				},
				{
					show = true,
					info = arg_8_0.nodeInfos.gametitle
				}
			},
			AutoPlayBootIn = {
				{
					hide = true,
					info = arg_8_0.nodeInfos.hud
				},
				{
					hide = true,
					info = arg_8_0.nodeInfos.resume_floating
				},
				{
					hide = true,
					info = arg_8_0.nodeInfos.thumbnails
				}
			},
			AutoPlayOut = {
				{
					hide = true,
					info = arg_8_0.nodeInfos.cardlist
				},
				{
					hide = true,
					info = arg_8_0.nodeInfos.gametitle
				},
				{
					show = true,
					info = arg_8_0.nodeInfos.cardlist
				},
				{
					enable = true,
					show = true,
					info = arg_8_0.nodeInfos.gametitle
				},
				{
					show = true,
					info = arg_8_0.nodeInfos.hud
				},
				{
					show = true,
					info = arg_8_0.nodeInfos.resume_floating
				},
				{
					show = true,
					info = arg_8_0.nodeInfos.thumbnails
				}
			},
			AutoPlayBootOut = {
				{
					hide = true,
					info = arg_8_0.nodeInfos.cardlist
				},
				{
					hide = true,
					info = arg_8_0.nodeInfos.gametitle
				},
				{
					show = true,
					info = arg_8_0.nodeInfos.hud
				},
				{
					show = true,
					info = arg_8_0.nodeInfos.resume_floating
				},
				{
					show = true,
					info = arg_8_0.nodeInfos.thumbnails
				},
				{
					show = true,
					info = arg_8_0.nodeInfos.cardlist
				},
				{
					show = true,
					info = arg_8_0.nodeInfos.gametitle
				}
			}
		}
	end
end

function CloverTransition.updateNodeAlpha(arg_9_0)
	local var_9_0 = arg_9_0.alpha

	if not var_9_0 then
		-- block empty
	elseif var_9_0 > 1 then
		var_9_0 = 1
	elseif var_9_0 < 0 then
		var_9_0 = 0
	else
		var_9_0 = 1 + math.sin(-math.pi * 0.5 + var_9_0 * math.pi * 0.5)
		var_9_0 = 1 + math.sin(-math.pi * 0.5 + var_9_0 * math.pi * 0.5)
	end

	local var_9_1 = arg_9_0.hideNodes[arg_9_0.mode]

	if not var_9_1 then
		print("ERROR: no exists mode=" .. arg_9_0.mode)

		return
	end

	if arg_9_0.seq == 1 then
		for iter_9_0, iter_9_1 in pairs(var_9_1) do
			if iter_9_1.hide and var_9_0 then
				local var_9_2 = iter_9_1.info.node:getLocalPositionY()

				if not iter_9_1.info.initY then
					iter_9_1.info.initY = var_9_2
				end

				local var_9_3 = iter_9_1.info.initY

				if iter_9_1.info.startY then
					var_9_3 = iter_9_1.info.startY
				end

				local var_9_4 = var_9_3 + var_9_0 * (iter_9_1.y or iter_9_1.info.y)

				iter_9_1.info.node:setLocalPositionY(var_9_4)

				if var_9_0 == 1 and iter_9_1.info.node:isEnabled() then
					iter_9_1.info.node:disable()

					iter_9_1.info.disabled = true
				end
			end
		end
	elseif arg_9_0.seq == 3 then
		for iter_9_2, iter_9_3 in pairs(var_9_1) do
			if iter_9_3.show and var_9_0 then
				local var_9_5 = iter_9_3.info.node:getLocalPositionY()

				if not iter_9_3.info.initY then
					iter_9_3.info.initY = var_9_5
				end

				local var_9_6 = iter_9_3.y or iter_9_3.info.y
				local var_9_7 = iter_9_3.info.initY + var_9_0 * var_9_6

				iter_9_3.info.node:setLocalPositionY(var_9_7)

				if iter_9_3.enable then
					iter_9_3.info.node:enable()
				end

				if iter_9_3.info.disabled then
					iter_9_3.info.node:enable()

					iter_9_3.info.disabled = false
				end
			end
		end
	end
end

function CloverTransition.updateTransition(arg_10_0, arg_10_1)
	if not arg_10_0.seq then
		arg_10_0.alpha = 0

		arg_10_0:updateNodeAlpha(0)

		arg_10_0.seq = 1
	elseif arg_10_0.seq == 1 then
		arg_10_0.alpha = arg_10_0.alpha + arg_10_1 / CloverConst.TRANSITION.DURATION

		arg_10_0:updateNodeAlpha()

		if arg_10_0.alpha > 1 then
			arg_10_0.seq = 2
		end
	elseif arg_10_0.seq == 2 then
		arg_10_0.callback_middle()

		arg_10_0.alpha = 1
		arg_10_0.seq = 3

		arg_10_0:clearRelative()
	elseif arg_10_0.seq == 3 then
		arg_10_0.alpha = arg_10_0.alpha - arg_10_1 / CloverConst.TRANSITION.DURATION

		arg_10_0:updateNodeAlpha()

		if arg_10_0.alpha < 0 then
			arg_10_0.seq = 4
		end
	elseif arg_10_0.seq == 4 then
		arg_10_0.callback_final()

		arg_10_0.alpha = nil

		arg_10_0:updateNodeAlpha()

		arg_10_0.is_running = false
		arg_10_0.seq = nil
	end
end
