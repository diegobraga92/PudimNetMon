import { useRef, useState } from 'react'
import { AlertTriangle, BellRing, Pencil, Plus, Trash2 } from 'lucide-react'
import type { AlertOp, AlertRule, Severity } from '../../types'
import { CHECK_TYPE_LABELS } from '../../lib/constants'
import { useAlertRules, useDeleteAlertRule, useUpsertAlertRule } from '../../hooks/useAlertRules'
import { Badge, type BadgeVariant } from '../ui/Badge'
import { Button } from '../ui/Button'
import { Card } from '../ui/Card'
import { Dialog } from '../ui/Dialog'
import { EmptyState } from '../ui/EmptyState'
import { Input } from '../ui/Input'
import { ListSkeleton } from '../ui/LoadingSkeleton'
import { Select } from '../ui/Select'
import { Switch } from '../ui/Switch'
import { useToast } from '../ui/toast'

const CHECK_TYPE_OPTIONS = [
  { value: 'all', label: 'All check types' },
  ...Object.entries(CHECK_TYPE_LABELS).map(([value, label]) => ({ value, label })),
]

const SEVERITY_OPTIONS: { value: Severity; label: string }[] = [
  { value: 'info', label: 'Info' },
  { value: 'warning', label: 'Warning' },
  { value: 'critical', label: 'Critical' },
]

const METRIC_OPTIONS = [
  { value: 'latency_ms', label: 'Latency (ms)' },
  { value: 'packet_loss_pct', label: 'Packet loss (%)' },
  { value: 'jitter_ms', label: 'Jitter (ms)' },
  { value: 'rtt_ms', label: 'Round-trip time (ms)' },
  { value: 'status_code', label: 'HTTP status code' },
]

const OP_OPTIONS: { value: AlertOp; label: string }[] = [
  { value: '>', label: 'above (>)' },
  { value: '<', label: 'below (<)' },
]

interface RuleForm {
  id: string
  name: string
  agentId: string
  target: string
  checkType: string
  metric: string
  op: AlertOp
  threshold: string
  repeatSec: string
  severity: Severity
  onFailure: boolean
}

type RuleFormErrors = Partial<Record<keyof RuleForm, string>>

const DEFAULT_FORM: RuleForm = {
  id: '',
  name: '',
  agentId: '',
  target: '',
  checkType: 'all',
  metric: 'latency_ms',
  op: '>',
  threshold: '',
  repeatSec: '300',
  severity: 'warning',
  onFailure: false,
}

function formFromRule(rule: AlertRule): RuleForm {
  return {
    id: rule.id,
    name: rule.name,
    agentId: rule.agent_id,
    target: rule.target,
    checkType: rule.check_type || 'all',
    metric: rule.metric,
    op: rule.op,
    threshold: String(rule.threshold),
    repeatSec: String(rule.repeat_interval_sec),
    severity: rule.severity,
    onFailure: rule.on_failure,
  }
}

function ruleFromForm(form: RuleForm): AlertRule {
  return {
    id: form.id.trim(),
    name: form.name.trim(),
    agent_id: form.agentId.trim(),
    check_type: form.checkType === 'all' ? '' : form.checkType,
    target: form.target.trim(),
    metric: form.onFailure ? '' : form.metric,
    op: form.op,
    threshold: Number(form.threshold) || 0,
    repeat_interval_sec: Number(form.repeatSec) || 0,
    severity: form.severity,
    on_failure: form.onFailure,
  }
}

function validateForm(form: RuleForm): RuleFormErrors {
  const errors: RuleFormErrors = {}
  if (!form.id.trim()) {
    errors.id = 'Rule id is required'
  } else if (!/^[a-zA-Z0-9][a-zA-Z0-9._-]{0,63}$/.test(form.id.trim())) {
    errors.id = 'Use letters, digits, dots, dashes or underscores'
  }
  if (!form.name.trim()) errors.name = 'Name is required'
  if (!form.onFailure) {
    if (!form.metric) errors.metric = 'Pick a metric field'
    const threshold = Number(form.threshold)
    if (form.threshold.trim() === '' || !Number.isFinite(threshold)) {
      errors.threshold = 'Threshold must be a number'
    } else if (threshold < 0) {
      errors.threshold = 'Threshold cannot be negative'
    }
  }
  const repeat = Number(form.repeatSec)
  if (form.repeatSec.trim() === '' || !Number.isInteger(repeat) || repeat < 0 || repeat > 86400) {
    errors.repeatSec = 'Whole seconds between 0 and 86400'
  }
  return errors
}

function severityBadge(severity: Severity): BadgeVariant {
  if (severity === 'critical') return 'critical'
  if (severity === 'warning') return 'warning'
  return 'info'
}

/** Human readable condition, e.g. "tcp_connect latency_ms > 500 ms". */
function conditionLabel(rule: AlertRule): string {
  if (rule.on_failure) return 'fires when the probe fails'
  const metricLabel = METRIC_OPTIONS.find((m) => m.value === rule.metric)?.label ?? rule.metric
  return `${metricLabel} ${rule.op} ${rule.threshold}`
}

export function AlertRulesPanel() {
  const { data, isLoading } = useAlertRules()
  const upsert = useUpsertAlertRule()
  const remove = useDeleteAlertRule()
  const { toast } = useToast()
  const [editor, setEditor] = useState<AlertRule | 'new' | null>(null)
  const [confirmId, setConfirmId] = useState<string | null>(null)
  const confirmTimer = useRef<number | null>(null)

  const rules = data?.rules ?? []
  const saving = upsert.isPending

  const openConfirm = (rule: AlertRule) => {
    if (confirmId === rule.id) return // second click handled on the row button
    if (confirmTimer.current) window.clearTimeout(confirmTimer.current)
    setConfirmId(rule.id)
    confirmTimer.current = window.setTimeout(() => setConfirmId(null), 3000)
  }

  const cancelConfirm = () => {
    if (confirmTimer.current) window.clearTimeout(confirmTimer.current)
    setConfirmId(null)
  }

  const handleSave = (rule: AlertRule, isNew: boolean) => {
    upsert.mutate(rule, {
      onSuccess: (data) => {
        setEditor(null)
        if (data.success === false) {
          toast({
            title: 'Rule not saved',
            description: data.error || 'Unknown error',
            variant: 'error',
          })
          return
        }
        toast({
          title: isNew ? 'Rule created' : 'Rule updated',
          description: data.persisted
            ? `${rule.name} is active and persisted to disk.`
            : `${rule.name} is active (not persisted — no rules file configured).`,
          variant: 'success',
        })
      },
      onError: (err) => {
        toast({
          title: 'Could not save rule',
          description: err instanceof Error ? err.message : 'Try again in a moment.',
          variant: 'error',
        })
      },
    })
  }

  const handleDelete = (rule: AlertRule) => {
    remove.mutate(rule.id, {
      onSuccess: (data) => {
        cancelConfirm()
        if (data.success === false) {
          toast({ title: 'Rule not deleted', description: data.error, variant: 'error' })
          return
        }
        toast({ title: 'Rule deleted', description: `${rule.name} removed.`, variant: 'success' })
      },
      onError: (err) => {
        cancelConfirm()
        toast({
          title: 'Could not delete rule',
          description: err instanceof Error ? err.message : 'Try again in a moment.',
          variant: 'error',
        })
      },
    })
  }

  return (
    <section aria-labelledby="alert-rules-heading" className="space-y-3">
      <div className="flex flex-wrap items-center justify-between gap-2">
        <div>
          <h2 id="alert-rules-heading" className="text-base font-semibold text-fg">
            Alert Rules
          </h2>
          <p className="text-xs text-fg-muted">
            Evaluated against every incoming metric — changes apply immediately.
          </p>
        </div>
        <Button size="sm" onClick={() => setEditor('new')} disabled={saving}>
          <Plus className="size-3.5" aria-hidden="true" />
          Add rule
        </Button>
      </div>

      {isLoading ? (
        <Card className="p-4">
          <ListSkeleton rows={3} />
        </Card>
      ) : rules.length === 0 ? (
        <EmptyState
          icon={<BellRing className="size-8" aria-hidden="true" />}
          title="No alert rules yet"
          description="Rules watch probe metrics (or probe failures) and raise alerts when a threshold is crossed."
          action={
            <Button size="sm" onClick={() => setEditor('new')}>
              <Plus className="size-3.5" aria-hidden="true" />
              Add your first rule
            </Button>
          }
        />
      ) : (
        <Card className="divide-y divide-border overflow-hidden">
          {rules.map((rule) => {
            const pendingDelete = confirmId === rule.id
            return (
              <div key={rule.id} className="flex flex-wrap items-center gap-x-4 gap-y-2 p-4">
                <div className="min-w-0 flex-1 basis-52">
                  <div className="flex flex-wrap items-center gap-2">
                    <strong className="truncate text-sm text-fg">{rule.name}</strong>
                    <Badge variant={severityBadge(rule.severity)}>{rule.severity}</Badge>
                    {rule.on_failure && (
                      <Badge variant="neutral">
                        <AlertTriangle className="size-3" aria-hidden="true" />
                        on failure
                      </Badge>
                    )}
                  </div>
                  <p className="mt-0.5 text-xs text-fg-muted">
                    <span className="font-medium text-fg-subtle">{conditionLabel(rule)}</span>
                    <span className="mx-1.5 text-fg-subtle">·</span>
                    {rule.check_type ? rule.check_type : 'all checks'}
                    <span className="mx-1.5 text-fg-subtle">·</span>
                    {rule.agent_id || 'all agents'}
                    {rule.target && (
                      <>
                        <span className="mx-1.5 text-fg-subtle">→</span>
                        {rule.target}
                      </>
                    )}
                    <span className="mx-1.5 text-fg-subtle">·</span>
                    repeat {rule.repeat_interval_sec}s
                  </p>
                </div>
                <div className="flex items-center gap-2">
                  {pendingDelete ? (
                    <>
                      <Button
                        variant="danger"
                        size="sm"
                        loading={remove.isPending}
                        onClick={() => handleDelete(rule)}
                      >
                        <Trash2 className="size-3.5" aria-hidden="true" />
                        Confirm
                      </Button>
                      <Button variant="ghost" size="sm" onClick={cancelConfirm}>
                        Keep
                      </Button>
                    </>
                  ) : (
                    <>
                      <Button
                        variant="ghost"
                        size="sm"
                        aria-label={`Edit ${rule.name}`}
                        disabled={saving}
                        onClick={() => setEditor(rule)}
                      >
                        <Pencil className="size-3.5" aria-hidden="true" />
                        Edit
                      </Button>
                      <Button
                        variant="ghost"
                        size="sm"
                        aria-label={`Delete ${rule.name}`}
                        disabled={remove.isPending}
                        onClick={() => openConfirm(rule)}
                      >
                        <Trash2 className="size-3.5 text-critical" aria-hidden="true" />
                      </Button>
                    </>
                  )}
                </div>
              </div>
            )
          })}
        </Card>
      )}

      {editor && (
        <RuleEditorDialog
          rule={editor === 'new' ? undefined : editor}
          saving={saving}
          onClose={() => setEditor(null)}
          onSave={handleSave}
        />
      )}
    </section>
  )
}
interface RuleEditorDialogProps {
  rule?: AlertRule
  saving: boolean
  onClose: () => void
  onSave: (rule: AlertRule, isNew: boolean) => void
}

function RuleEditorDialog({ rule, saving, onClose, onSave }: RuleEditorDialogProps) {
  const isNew = !rule
  const [form, setForm] = useState<RuleForm>(() => (rule ? formFromRule(rule) : DEFAULT_FORM))
  const [errors, setErrors] = useState<RuleFormErrors>({})

  const setField = <K extends keyof RuleForm>(key: K, value: RuleForm[K]) => {
    setForm((prev) => ({ ...prev, [key]: value }))
    setErrors((prev) => ({ ...prev, [key]: undefined }))
  }

  const handleSubmit = () => {
    const nextErrors = validateForm(form)
    if (Object.values(nextErrors).some(Boolean)) {
      setErrors(nextErrors)
      return
    }
    onSave(ruleFromForm(form), isNew)
  }

  return (
    <Dialog
      open
      onOpenChange={(open) => {
        if (!open && !saving) onClose()
      }}
      title={isNew ? 'Add alert rule' : `Edit “${rule.name}”`}
      description="Rules are evaluated against every incoming probe result."
      maxWidthClass="max-w-2xl"
    >
      <div className="grid gap-4">
        <div className="grid gap-4 sm:grid-cols-2">
          <Input
            label="Rule id"
            value={form.id}
            disabled={!isNew}
            onChange={(e) => setField('id', e.target.value)}
            placeholder="high-tcp-latency"
            hint={isNew ? 'Short unique slug. Cannot change later.' : undefined}
            error={errors.id}
          />
          <Input
            label="Display name"
            value={form.name}
            onChange={(e) => setField('name', e.target.value)}
            placeholder="High TCP Connect Latency"
            error={errors.name}
          />
        </div>

        <div className="grid gap-4 sm:grid-cols-2">
          <Input
            label="Agent id (optional)"
            value={form.agentId}
            onChange={(e) => setField('agentId', e.target.value)}
            placeholder="All agents"
            hint="Leave blank to match every agent."
          />
          <Input
            label="Target (optional)"
            value={form.target}
            onChange={(e) => setField('target', e.target.value)}
            placeholder="example.com:443"
            hint="Leave blank to match every target."
          />
        </div>

        <div className="grid gap-4 sm:grid-cols-2">
          <div className="flex flex-col gap-1.5">
            <span className="text-xs font-medium text-fg-muted">Check type</span>
            <Select
              ariaLabel="Check type"
              value={form.checkType}
              onValueChange={(v) => setField('checkType', v)}
              options={CHECK_TYPE_OPTIONS}
            />
          </div>
          <div className="flex flex-col gap-1.5">
            <span className="text-xs font-medium text-fg-muted">Severity</span>
            <Select
              ariaLabel="Severity"
              value={form.severity}
              onValueChange={(v) => setField('severity', v as Severity)}
              options={SEVERITY_OPTIONS}
            />
          </div>
        </div>

        <Switch
          label="Fire when the probe fails (ignore threshold rules below)"
          checked={form.onFailure}
          onCheckedChange={(v) => setField('onFailure', v)}
        />

        {!form.onFailure && (
          <div className="grid gap-4 sm:grid-cols-[1fr_auto_1fr]">
            <div className="flex flex-col gap-1.5">
              <span className="text-xs font-medium text-fg-muted">Metric</span>
              <Select
                ariaLabel="Metric"
                value={form.metric}
                onValueChange={(v) => setField('metric', v)}
                options={METRIC_OPTIONS}
              />
              {errors.metric && <p className="text-xs text-critical">{errors.metric}</p>}
            </div>
            <div className="flex flex-col gap-1.5">
              <span className="text-xs font-medium text-fg-muted">Condition</span>
              <Select
                ariaLabel="Condition"
                value={form.op}
                onValueChange={(v) => setField('op', v as AlertOp)}
                options={OP_OPTIONS}
              />
            </div>
            <Input
              label="Threshold"
              type="number"
              min={0}
              step="any"
              value={form.threshold}
              onChange={(e) => setField('threshold', e.target.value)}
              placeholder="500"
              error={errors.threshold}
            />
          </div>
        )}

        <Input
          label="Repeat notification every (seconds)"
          type="number"
          min={0}
          max={86400}
          step={1}
          value={form.repeatSec}
          onChange={(e) => setField('repeatSec', e.target.value)}
          hint="While the alert stays firing, a new notification is sent at most this often. 0 disables repeats."
          error={errors.repeatSec}
        />

        <div className="mt-2 flex flex-wrap justify-end gap-2">
          <Button variant="ghost" onClick={onClose} disabled={saving}>
            Cancel
          </Button>
          <Button onClick={handleSubmit} loading={saving}>
            {isNew ? 'Create rule' : 'Save changes'}
          </Button>
        </div>
      </div>
    </Dialog>
  )
}
