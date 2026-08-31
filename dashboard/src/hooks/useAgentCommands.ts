import { useMutation, useQuery } from '@tanstack/react-query'
import { apiGet, apiPost } from '../lib/api'
import type {
  AgentCommand,
  AgentCommandsResponse,
  AgentInfo,
  CommandResult,
} from '../types'

/** Fetches the pre-set (whitelisted) command catalog for a single agent. */
export function useAgentCommands(agent: AgentInfo | null) {
  return useQuery({
    queryKey: ['agent-commands', agent?.agent_id],
    queryFn: async (): Promise<AgentCommandsResponse> =>
      apiGet('/api/agents/commands', { agent_id: agent?.agent_id }),
    enabled: agent != null,
    staleTime: 30_000,
  })
}

/** Runs one pre-set command on an agent and returns the structured result. */
export function useRunAgentCommand() {
  return useMutation({
    mutationFn: async (input: {
      agent: AgentInfo
      command: AgentCommand
    }): Promise<CommandResult> =>
      apiPost('/api/agents/command', {
        agent_id: input.agent.agent_id,
        command_id: input.command.command_id,
        params: {},
      }),
  })
}
