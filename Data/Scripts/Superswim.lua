----- GLOBAL VARIABLES -----
endX = 0.0
endY = 0.0
startFrame = 0
swimOffset = 16

currStickAngle = 0
currMainStickX = 0
currMainStickY = 0
currFrame = 0
releaseFrame = 0

startingSwim = true

rad = 0.0174532925


function GetX()
	return ReadValueFloat(0x803A7AF4 - 0x7FFF0000)
end

function GetY()
	return (-1) * ReadValueFloat(0x803A7AFC - 0x7FFF0000)
end

function GetAir()
	return ReadValue16(0x803ADC62 - 0x7FFF0000)
end

function GetSpeed()
	local speedAdress = GetPointerNormal(0x3AD860)
	speedAdress = speedAdress + 0x34E4
	return ReadValueFloat(speedAdress)
end

function GetCameraAngle()
	local xPos = ReadValueFloat(0x803D0F40 - 0x7FFF0000)
	local yPos = ReadValueFloat(0x803D0F38 - 0x7FFF0000)
	return GetAngle(0,0,xPos, yPos)
end

function GetFacingDirection()
	local facingDir = ReadValue16(0x803A7B18 - 0x7FFF0000)
	return ((facingDir/182.0444444444444) - 90)%360
end

function GetAngle(x1,y1,x2,y2) -- returns the angle (in degrees) of the coordinates (x1,y1) and (x2,y2)
	local dx = x2 - x1
	local dy = y2 - y1
	if dx == 0 and dy == 0 then
		return 0
	end
	local angle = math.deg(math.asin( dy / math.sqrt(dx^2 + dy^2 ) ) )
	if x2 < x1 then
		angle = 180 - angle
	elseif y2 < y1 then
		angle = 360 + angle
	end
	return angle
end

function GetDistance(x1, y1, x2, y2)
         local dx = x2 - x1
         local dy = y2 - y1
         return math.sqrt(dx^2 + dy^2 )
end

function GetAverageAngle(a, b)          -- returns the average of two angles
         local avg
         if (math.abs(a - b) >= 180) then
            avg = (a + b)/2
	        avg = (avg + 180)%360
         else
	         avg = (a+b)/2
         end
         return avg
end

function SubtractAngles(a, b)          -- subtracts two angles
         if(a > b) then
               return a - b
         else
               return  (a - b) + 360
         end
end

function CheckAngle(min, max, myAngle) -- checks to see if the specified angle is within the boundaries of the min and max
         local i = min
         while true do
             local angleCheck = math.abs(i - myAngle)
             if angleCheck > 0 and angleCheck <= 1 then
                 return true
             end
             if math.abs(i - max) > 0 and math.abs(i - max) <= 1 then
                 break
             end
             i = (i + 1)%360
         end
         return false
end

function GetArrivalFrame(a, b, c)     -- uses quadratic formula to find earliest arrive frame
         a = (0.5) * a
         p1 = (-1 * b)
         p2 = b^2
         p3 = (4 * a * c)
         p4 = (2 * a)

         if (p2 - p3) < 0 then        -- not enough air to make the swim, returns arbritrarily large value
            return 100000
         end
         local solution1 = math.ceil( (p1 - math.sqrt(p2 - p3) )/p4 )
         local solution2 = math.ceil( (p1 + math.sqrt(p2 - p3) )/p4 )
         if (solution1 >= 0 and solution2 >= 0) then
            if solution1 >= solution2 then
                return solution2
            else
                return solution1
            end
         end

         if (solution1 < 0) then
             return solution2
         else
             return solution1
         end
end


function PredictX(i)
         if (i==0) then
               return (-1) * GetSpeed()
         else
               local mySpeed = (-1) * PredictAcceleration() * i
               local myAngle = -1^i * swimOffset
               myAngle = (myAngle + 180)%180

               return (math.cos(myAngle * rad) * mySpeed) + PredictX(i - 1)
         end
end

function PredictY(i)
         if (i==0) then
               return (-1) * GetSpeed()
         else
               local mySpeed = (-1) * PredictAcceleration() * i
               local myAngle = -1^i * swimOffset
               myAngle = (myAngle + 180)%180


               return (math.sin(myAngle * rad) * mySpeed) + PredictY(i - 1)
         end
end


function DistanceTraveled(i)
         return math.sqrt( PredictX(i)^2 + PredictY(i)^2 )
end

function PredictAcceleration()
         return (0.00242071781259067 * swimOffset^2 ) - (0.0338464925589466 * swimOffset) - 2.88259775256355
end

function PredictBestFrame() -- predicts which frame/speed needs to be obtained for best arrival time
		local distance = GetDistance(GetX(), GetY(), endX, endY)
		local accel = 0.4041
		local bestFrame = 10000
		local bestArrive = 10000
		for i = 0,GetAir() do  -- predicts arrival time for every frame
			local newDistance = distance - DistanceTraveled(i)
			local speed = (PredictAcceleration() * i) * 0.73
			local arrive = GetArrivalFrame(accel, speed, newDistance)
			arrive = arrive + i
			if(arrive < bestArrive and arrive ~= i) then
				bestFrame = i
				bestSpeed = speed
				bestArrive = arrive
			end
		end
		if bestArrive > GetAir() then
			MsgBox("Not enough air for this swim!")
			AbortSwim()
			return 0
		end
		return bestFrame
end

function SetDirection(val)      -- converts the angle specified into the joystick layout
        if GetFrameCount() == currFrame then
			return
		end
		currFrame = GetFrameCount()
		currStickAngle = val
		local cameraOffset = GetCameraAngle()
        val = (SubtractAngles(val, cameraOffset) + 90 )%360

         x = math.cos(val * rad) * 128
         y = math.sin(val * rad) * 128

         if (math.ceil(x) - x <= 0.5) then
             x = math.ceil(x)
         else
             x = math.floor(x)
         end
         if (math.ceil(y) - y <= 0.5) then
             y = math.ceil(y)
         else
             y = math.floor(y)
         end

         x = x + 128
         y = y + 128

         if (x >= 256) then
            x = 255
         end
         if (y >= 256) then
            y = 255
         end

         SetMainStickX(x)
		 SetMainStickY(y)
		 currMainStickX = x
		 currMainStickY = y
end

function ReorientateLink()
	SetCStickX(128)
	SetCStickY(0)
	if GetAir() == 0 then
		return
	elseif GetSpeed() >= 0 then
		SetDirection((currStickAngle + 180)%360)
		-- (endAngle - (swimOffset/2))%360
		--(endAngle + 180)%360
	else
		local endAngle = ( ((GetAngle(GetX(), GetY(), endX, endY) - 90) + swimOffset)%360)
		local nextAngle = (endAngle + 180)%360
		if CheckAngle( (nextAngle - swimOffset)%360, nextAngle, currStickAngle) == true then
			startingSwim = false
			startFrame = GetFrameCount()

			releaseFrame = GetAir() - PredictBestFrame()
			return
		else
			SetDirection((currStickAngle + (180 - (swimOffset * 1.5) ) )%360 )
		end
	end
end

function ReleaseSwim()          -- Positions Link towards the end destination
		local endAngle = GetAngle(GetX(), GetY(), endX, endY)


		if GetAir() == releaseFrame + 4 then
			local myAngle = (currStickAngle + 180)%360
			local avgAngle = GetAverageAngle(myAngle, endAngle)

			myAngle = GetAverageAngle(myAngle, avgAngle)
			SetDirection(myAngle)
		elseif GetAir() == releaseFrame + 3 then
			endAngle = (endAngle + 180)%360
			local myAngle = (currStickAngle + 180)%360
			local avgAngle = GetAverageAngle(myAngle, endAngle)

			myAngle = GetAverageAngle(myAngle, avgAngle)
			SetDirection(myAngle)
		elseif GetAir() == releaseFrame + 2 then
			local myAngle = (currStickAngle + 180)%360
			local avgAngle = GetAverageAngle(myAngle, endAngle)

			myAngle = GetAverageAngle(myAngle, avgAngle)
			SetDirection(myAngle)
		elseif GetAir() == releaseFrame + 1 then
			SetDirection( (endAngle + 180)%360)
		end


end

function KeepSwimming()
	local cameraOffset = GetCameraAngle()
	local myAngle = GetAngle(GetX(), GetY(), endX, endY)
	myAngle = (myAngle + 180)%360
	myAngle= (SubtractAngles(myAngle, cameraOffset) + 90 )%360

	if CheckAngle(0,44, myAngle) then
		SetMainStickX(146)
		SetMainStickY(128)

	elseif CheckAngle(45,89, myAngle) then
		SetMainStickX(146)
		SetMainStickY(146)

	elseif CheckAngle(90,134, myAngle) then
		SetMainStickX(128)
		SetMainStickY(146)

	elseif CheckAngle(135, 179, myAngle) then
		SetMainStickX(110)
		SetMainStickY(146)

	elseif CheckAngle(180, 224, myAngle) then
		SetMainStickX(110)
		SetMainStickY(128)

	elseif CheckAngle(225, 269, myAngle) then
		SetMainStickX(110)

		SetMainStickY(110)
	elseif CheckAngle(270, 314, myAngle) then
		SetMainStickX(128)
		SetMainStickY(110)

	elseif CheckAngle(315, 359, myAngle) then
		SetMainStickX(146)
		SetMainStickY(110)
	end
end



function Swim()                -- Turns Link left or right depending on frame
         local myAngle = GetAngle(GetX(),GetY(),endX,endY)

         if (GetFrameCount()-startFrame)%2 == 0 then -- Left
             myAngle = (myAngle + 90)%360
             myAngle = (myAngle + swimOffset)%360
         else
             myAngle = (myAngle - 90)%360            -- Right
             myAngle = (myAngle - swimOffset)%360
         end
         SetDirection(myAngle)
end


function startSwim(x, z)
	endX = x
	endY = (-1) * z
	currStickAngle = GetFacingDirection()
	currFrame = GetFrameCount()
end

function cancelSwim()
	MsgBox("Script Ended!")
end

function updateSwim()
	if startingSwim == true then
		ReorientateLink()
	elseif GetAir() > releaseFrame + 4 then
		Swim()
	elseif GetAir() <= releaseFrame + 4 and GetAir() > releaseFrame then
		ReleaseSwim()
	elseif GetAir() > 0 then
		KeepSwimming()
	else
		AbortSwim()
	end

end


