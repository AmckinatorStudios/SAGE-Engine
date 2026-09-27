-- Твин из скрипта: объект мягко «дышит», а по пробелу подпрыгивает
-- последовательностью — вверх, пауза, вниз с отскоком.

size = 1.15        -- до какого масштаба дышать
period = 0.6       -- полпериода, секунды

function Start()
    Tween.to(self, "scale", size, period, Ease.InOutSine):ping_pong()
end

function OnKeyDown(key)
    if key ~= "Space" or jumping then return end
    jumping = true
    local y = self.transform.position.y
    Tween.to(self, "position", self.transform.position + Vector3.up() * 2, 0.3, Ease.OutCubic)
        :wait(0.1)
        :then_to("position", Vector3.new(self.transform.position.x, y, self.transform.position.z), 0.5, Ease.OutBounce)
        :on_complete(function() jumping = false end)
end
