-- Окружение из скрипта: тип неба, туман, цикл дня и ночи (время идёт в Play).
local E = {}

function E:before_each()
    self.env = sage.light.Get()
    self.saved = {
        mode = self.env.Skybox.Mode, cycle = self.env.Cycle.Enabled, time = self.env.Cycle.Time,
        len = self.env.Cycle.DayLengthMinutes, fog = self.env.Fog.Enabled,
    }
end

function E:after_each()
    local s = self.saved
    self.env.Skybox.Mode = s.mode
    self.env.Cycle.Enabled = s.cycle
    self.env.Cycle.Time = s.time
    self.env.Cycle.DayLengthMinutes = s.len
    self.env.Fog.Enabled = s.fog
end

function E:test_sky_mode_by_word()
    self.env.Skybox.Mode = "solid"
    Test.eq(self.env.Skybox.Mode, "solid")
    self.env.Skybox.Mode = "nonsense"
    Test.eq(self.env.Skybox.Mode, "solid", "неизвестное слово поменяло тип неба")
    self.env.Skybox:SetFace(3, "assets/sky/up.png")
    Test.eq(self.env.Skybox:GetFace(3), "assets/sky/up.png")
end

function E:test_fog_mode_by_word()
    self.env.Fog.Enabled = true
    self.env.Fog.Mode = "height"
    Test.eq(self.env.Fog.Mode, "height")
    self.env.Fog.Mode = "linear"
    Test.eq(self.env.Fog.Mode, "linear")
end

function E:test_day_night_cycle_runs_in_play()
    local c = self.env.Cycle
    c.Enabled = true
    c.RunInPlay = true
    c.Time = 10
    c.DayLengthMinutes = 0.1   -- сутки за 6 секунд: 4 часа в секунду
    c.Speed = 1
    Test.wait(0.5)
    Test.expect(c.Time > 11, "время суток не пошло: " .. c.Time)
    c.Speed = 0
    local t = c.Time
    Test.wait(0.3)
    Test.near(c.Time, t, 1e-4, "время шло при Speed = 0")
end

return E
