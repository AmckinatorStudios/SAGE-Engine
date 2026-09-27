-- Жизненный цикл скрипта в настоящем Play: Start, Update, FixedUpdate,
-- LateUpdate, публичные поля, время.
local L = {}

L.public = {
    speed = field.number(3.5, 0, 10),
    label = field.string("hi"),
    lives = field.integer(3),
}

function L:Start()
    self.started = (self.started or 0) + 1
    self.updates, self.fixed, self.late = 0, 0, 0
end
function L:Update(dt)
    self.updates = self.updates + 1
    self.lastDt = dt
    self.updateSeen = true
end
function L:FixedUpdate(dt)
    self.fixed = self.fixed + 1
    self.fixedDt = dt
end
function L:LateUpdate(dt)
    self.late = self.late + 1
    self.lateAfterUpdate = self.updateSeen
end

function L:test_start_ran_exactly_once()
    Test.eq(self.started, 1)
end

function L:test_update_runs_every_frame_with_dt()
    local before = self.updates
    Test.frames(3)
    Test.expect(self.updates >= before + 3, "Update пропускал кадры: " .. (self.updates - before))
    Test.expect(self.lastDt and self.lastDt > 0, "dt в Update не положительный")
end

function L:test_fixed_update_uses_the_fixed_step()
    Test.wait(0.5)
    Test.expect(self.fixed > 5, "FixedUpdate почти не шёл: " .. self.fixed)
    Test.near(self.fixedDt, Time.fixedDeltaTime, 1e-5)
end

function L:test_late_update_runs_after_update()
    Test.frames(2)
    Test.expect(self.late > 0, "LateUpdate не шёл")
    Test.expect(self.lateAfterUpdate, "LateUpdate раньше Update")
end

function L:test_public_fields_have_declared_defaults()
    Test.near(self.speed, 3.5)
    Test.eq(self.label, "hi")
    Test.eq(self.lives, 3)
end

function L:test_self_knows_its_object()
    Test.expect(self.gameObject:IsValid())
    Test.eq(self.gameObject.name, self.name)
    Test.eq(self:GetScript(), self, "GetScript своего объекта — не self")
end

function L:test_time_advances()
    local t0 = Time.time
    Test.wait(0.3)
    Test.expect(Time.time - t0 >= 0.25, "Time.time прошёл " .. (Time.time - t0))
    Test.expect(Time.deltaTime > 0)
end

function L:test_time_scale_zero_freezes_game_time()
    Time.timeScale = 0
    local t0, fixed0 = Time.time, self.fixed
    Test.frames(5)
    Time.timeScale = 1
    Test.near(Time.time, t0, 1e-6, "время шло при timeScale = 0")
    Test.eq(self.fixed, fixed0, "FixedUpdate шёл при timeScale = 0")
end

return L
