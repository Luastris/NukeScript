-- bob up and down. `self` = owning Atom, `dt` = frame delta.
-- `props` are exported and editable in the inspector (edits are seen here live).
local props = { speed = 2.0, height = 1.0 }
local t = 0
return {
  props = props,
  update = function(self, dt)
    t = t + dt
    self.transform.position.y = math.sin(t * props.speed) * props.height
  end
}
