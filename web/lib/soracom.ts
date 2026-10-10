/** Soracom SIM enrolment -- docs/SORACOM_DESIGN.md §5-6. Super-admin only. */

import { api } from "@/lib/api";

export interface SoracomSim {
  imsi: string;
  iccid?: string | null;
  status?: string | null;
  groupId?: string | null;
  enrolled: boolean;
  name?: string | null;
}

export interface SoracomSimsResponse {
  configured: boolean;
  group: string;
  groupId: string | null;
  sims: SoracomSim[];
}

export interface EnrolResult {
  ok: boolean;
  imsi: string;
  groupId: string;
}

export interface EnrolAllResult {
  ok: boolean;
  enrolled: string[];
  already: string[];
}

export function listSoracomSims(): Promise<SoracomSimsResponse> {
  return api.get<SoracomSimsResponse>("/admin/soracom/sims");
}

export function enrolSim(imsi: string): Promise<EnrolResult> {
  return api.post<EnrolResult>(`/admin/soracom/sims/${encodeURIComponent(imsi)}/enrol`);
}

export function enrolAllSims(): Promise<EnrolAllResult> {
  return api.post<EnrolAllResult>("/admin/soracom/enrol-all");
}

/** All but the last four digits hidden: "***********1234". */
export function maskImsi(imsi: string): string {
  return imsi.length <= 4 ? imsi : "*".repeat(imsi.length - 4) + imsi.slice(-4);
}
