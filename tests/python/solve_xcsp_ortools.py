#!/usr/bin/env python3
"""
使用 OR-Tools CP-SAT 求解器求解 XCSP 2.1 格式的约束满足问题
"""
import xml.etree.ElementTree as ET
import sys
import time
from ortools.sat.python import cp_model


def parse_domain(domain_str):
    """解析域字符串，返回值列表"""
    values = []
    for part in domain_str.strip().split():
        if '..' in part:
            # 范围格式: a..b
            start, end = map(int, part.split('..'))
            values.extend(range(start, end + 1))
        else:
            values.append(int(part))
    return values


def parse_tuples(tuple_str):
    """解析元组字符串，返回元组列表"""
    tuples = []
    for t in tuple_str.strip().split('|'):
        if t.strip():
            tuples.append(tuple(map(int, t.strip().split())))
    return tuples


def solve_xcsp(xml_path):
    """解析 XCSP 文件并用 OR-Tools 求解"""
    print(f"[OR-Tools] Parsing: {xml_path}")
    start_parse = time.time()

    tree = ET.parse(xml_path)
    root = tree.getroot()

    # 解析域
    domains = {}
    for domain in root.findall('.//domain'):
        name = domain.get('name')
        values = parse_domain(domain.text)
        domains[name] = values

    # 解析变量
    variables = {}
    var_order = []
    for var in root.findall('.//variable'):
        name = var.get('name')
        domain_name = var.get('domain')
        variables[name] = domains[domain_name]
        var_order.append(name)

    # 解析关系
    relations = {}
    for rel in root.findall('.//relation'):
        name = rel.get('name')
        semantics = rel.get('semantics')
        tuples = parse_tuples(rel.text) if rel.text else []
        relations[name] = {'semantics': semantics, 'tuples': tuples}

    # 解析约束
    constraints = []
    for cons in root.findall('.//constraint'):
        scope = cons.get('scope').split()
        ref = cons.get('reference')
        constraints.append({'scope': scope, 'relation': ref})

    parse_time = time.time() - start_parse
    print(f"[OR-Tools] Parse time: {parse_time:.3f}s")
    print(f"[OR-Tools] Variables: {len(variables)}, Constraints: {len(constraints)}, Relations: {len(relations)}")

    # 创建 CP-SAT 模型
    model = cp_model.CpModel()

    # 创建变量
    cp_vars = {}
    for var_name in var_order:
        domain = variables[var_name]
        cp_vars[var_name] = model.NewIntVarFromDomain(
            cp_model.Domain.FromValues(domain), var_name)

    # 添加约束
    for cons in constraints:
        scope = cons['scope']
        rel = relations[cons['relation']]
        semantics = rel['semantics']
        tuples = rel['tuples']

        if len(scope) == 2:
            var1, var2 = cp_vars[scope[0]], cp_vars[scope[1]]

            if semantics == 'supports':
                # 允许的元组
                model.AddAllowedAssignments([var1, var2], tuples)
            else:  # conflicts
                # 禁止的元组
                model.AddForbiddenAssignments([var1, var2], tuples)
        else:
            # 多元约束
            scope_vars = [cp_vars[s] for s in scope]
            if semantics == 'supports':
                model.AddAllowedAssignments(scope_vars, tuples)
            else:
                model.AddForbiddenAssignments(scope_vars, tuples)

    # 求解
    print("[OR-Tools] Solving...")
    start_solve = time.time()

    solver = cp_model.CpSolver()
    solver.parameters.max_time_in_seconds = 900  # 15分钟超时
    solver.parameters.log_search_progress = True

    status = solver.Solve(model)
    solve_time = time.time() - start_solve

    print(f"\n[OR-Tools] Solve time: {solve_time:.3f}s")
    print(f"[OR-Tools] Status: {solver.StatusName(status)}")

    if status == cp_model.OPTIMAL or status == cp_model.FEASIBLE:
        print(f"[OR-Tools] Solution found!")
        # 打印解（前20个变量）
        solution = []
        for var_name in var_order:
            solution.append(solver.Value(cp_vars[var_name]))

        print(f"[OR-Tools] Solution (first 20): {solution[:20]}")
        print(f"[OR-Tools] Full solution: {' '.join(map(str, solution))}")
        return True
    else:
        print(f"[OR-Tools] No solution found or timeout")
        return False


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python solve_xcsp_ortools.py <xcsp_file>")
        sys.exit(1)

    solve_xcsp(sys.argv[1])
