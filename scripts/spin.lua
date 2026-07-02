-- spin around the Y axis. `degPerSec` is editable in the inspector.
local props = { degPerSec = 90.0 }
local a = 0
return {
  props = props,
  update = function(self, dt)
    a = a + props.degPerSec * dt
    self.transform:setEuler(0, a, 0)
  end
}
