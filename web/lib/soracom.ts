/** Soracom SIM enrollment -- docs/SORACOM_DESIGN.md §5-6. Super-admin only. */

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

export interface EnrollResult {
  ok: boolean;
  imsi: string;
  groupId: string;
}

export interface EnrollAllResult {
  ok: boolean;
  enrolled: string[];
  already: string[];
}

export function listSoracomSims(): Promise<SoracomSimsResponse> {
  return api.get<SoracomSimsResponse>("/admin/soracom/sims");
}

export function enrollSim(imsi: string): Promise<EnrollResult> {
  return api.post<EnrollResult>(`/admin/soracom/sims/${encodeURIComponent(imsi)}/enroll`);
}

export function enrollAllSims(): Promise<EnrollAllResult> {
  return api.post<EnrollAllResult>("/admin/soracom/enroll-all");
}

/** All but the last four digits hidden: "***********1234". */
export function maskImsi(imsi: string): string {
  return imsi.length <= 4 ? imsi : "*".repeat(imsi.length - 4) + imsi.slice(-4);
}
